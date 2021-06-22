/*
Lie Group(SE3) (innovation-saturated) Extended Kalman filter LG (IS) EKF
constant velocity(differential kinematic)

Data:  2021.06.21 
Autor: Donghyun Sung sdh1259@snu.ac.kr
*/
#include <motion_filter/pre_process.hpp>

namespace motion_filter
{

VR::matrix_3_4 isometry3d2VRmsg(Eigen::Isometry3d T)
{
  Eigen::Matrix4d T_mat = T.matrix();

  VR::matrix_3_4 msg;

  Eigen::RowVector4d rv1, rv2, rv3;
  rv1 = T_mat.block(0, 0, 1, 4);
  rv2 = T_mat.block(1, 0, 1, 4);
  rv3 = T_mat.block(2, 0, 1, 4);
  

  std::vector<double> r1(&rv1[0], rv1.data() + rv1.cols()*rv1.rows());
  std::vector<double> r2(&rv2[0], rv2.data() + rv2.cols()*rv2.rows());
  std::vector<double> r3(&rv3[0], rv3.data() + rv3.cols()*rv3.rows());
  
  msg.firstRow = r1;
  msg.secondRow = r2;
  msg.thirdRow = r3;

  return msg;
}

PreProcess::PreProcess(ros::NodeHandle &nh, int tracker_id, double dt, bool verbose)
{
    tracker_id_ = tracker_id;
    dt_ = dt;
    verbose = verbose_;

    if (tracker_id_ < NUM_TRACKER)
        pose_pub_ = nh.advertise<VR::matrix_3_4>("/FTRACKER" + std::to_string(tracker_id), 100);
    else
        pose_pub_ = nh.advertise<VR::matrix_3_4>("/FHMD", 100);

    vel_pub_ = nh.advertise<std_msgs::Float64MultiArray>("/Fspvel" + std::to_string(tracker_id), 100);
    pos_quat_pub_ = nh.advertise<std_msgs::Float64MultiArray>("/Fposquat" + std::to_string(tracker_id), 100);

    std::string package_path = ros::package::getPath("motion_filter");
    std::string toml_path = package_path + "/config/filter_param.toml";
    parseToml(toml_path);

    Matrix6d I6 = Eigen::MatrixXd::Identity(6, 6);
    Matrix6d O6;
    O6.setZero();

    H_ << I6, O6;

    // std::cout<<"Measurement Jac" <<H_ <<std::endl;

}

PreProcess::~PreProcess()
{
    std::cout << "PreProcess Destructor" << std::endl;
}

Eigen::Isometry3d PreProcess::getTransform()
{
    Vector7d pos_quat = T_.coeffs();
    Eigen::Isometry3d T_iso;
    T_iso.translation() = pos_quat.head(3);
    Eigen::Quaterniond q(pos_quat(3), pos_quat(4), pos_quat(5), pos_quat(6));
    T_iso.linear() = q.normalized().toRotationMatrix();

    return T_iso;
}

 
void PreProcess::step(Eigen::Isometry3d T_m)
{
    if (is_first_)
    {
        is_first_ = false;
        T_ = manif::SE3d(T_m);
        V_.setZero();
        std::cout<<tracker_id_ <<' '<< algo[key_]<<" Filter Start"<<std::endl;
    }
    else
    {
        predict();
        if (key_ == 0)
            ekfUpdate(T_m);
        else if (key_ == 1)
            isekfUpdate(T_m);
        else
            std::logic_error("Unknown filter type");
    }
    publish();
}
void PreProcess::restart()
{
    sigma_ = sigma_init_;
    epsilon_ = epsilon_init_;
    is_first_ = true;
}

void PreProcess::predict()
{
    // ROS_INFO("Predict");
    T_ = T_.plus(manif::SE3Tangentd(V_ * dt_), J_T_, J_V_);

    Matrix12d F;
    Matrix12d Q;

    F << J_T_, J_V_ * dt_, 
         Matrix6d::Zero(), Matrix6d::Identity();
    Q << J_V_ * process_cov_ * J_V_.transpose() * dt_ * dt_, process_cov_ * J_V_.transpose() * dt_,
         J_V_ * process_cov_ * dt_, process_cov_;

    P_ = F * P_ * F.transpose() + Q;
}
void PreProcess::ekfUpdate(Eigen::Isometry3d T_m)
{
    // ROS_INFO("Update");
    manif::SE3d T_m_manif = manif::SE3d(T_m);
    Vector6d z = (T_m_manif - T_).coeffs();

    Matrix6d Z = H_ * P_ * H_.transpose() + N_;
    Matrix12x6d K = P_ * H_.transpose() * Z.inverse();

    Vector12d dx = K * z;
    // Vector12d dx = K * z_clip;

    T_ = T_ + manif::SE3Tangentd(dx.head(6));
    // Heuristic clip
    // Vector6d dx_clip = (dx.tail(6).array().min(sigma_.array()).cwiseMax(-sigma_.array())).matrix();
    V_ = V_ + dx.tail(6);

    P_ = P_ - K * Z * K.transpose();

}
void PreProcess::isekfUpdate(Eigen::Isometry3d T_m)
{
    // ROS_INFO("Update");
    manif::SE3d T_m_manif = manif::SE3d(T_m);
    Vector6d z = (T_m_manif - T_).coeffs();
    Vector6d z_clip = (z.array().min(sigma_.array().sqrt()).cwiseMax(-sigma_.array().sqrt())).matrix();

    Matrix6d Z = H_ * P_ * H_.transpose() + N_;
    Matrix12x6d K = P_ * H_.transpose() * Z.inverse();

    Vector12d dx = K * z_clip;

    T_ = T_ + manif::SE3Tangentd(dx.head(6));
    V_ = V_ + dx.tail(6);

    P_ = P_ - K * Z * K.transpose();
    dynamicClipUpdate(z);
}
void PreProcess::dynamicClipUpdate(Vector6d z)
{
    sigma_ = (lambda1_.array() * sigma_.array() + gamma1_.array() * epsilon_.array() * Eigen::exp(-epsilon_.array())).matrix();
    epsilon_ = (lambda2_.array() * epsilon_.array() + gamma2_.array() * z.array().square()).matrix();
}
void PreProcess::publish()
{
    //VR message default
    pose_pub_.publish(isometry3d2VRmsg(getTransform()));

    //pos quat
    std_msgs::Float64MultiArray posquat; //a.k.a std::vector<double>
    posquat.data.clear();
    posquat.data.resize(7);
    Eigen::VectorXd::Map(&posquat.data[0], 7) = T_.coeffs();
    pos_quat_pub_.publish(posquat);

    // //spatial velocity
    // std_msgs::Float64MultiArray spvel; //a.k.a std::vector<double>
    // spvel.data.clear();
    // spvel.data.resize(6);
    // Eigen::VectorXd::Map(&spvel.data[0], 6) = V_;
    // vel_pub_.publish(spvel);

}
void PreProcess::parseToml(std::string &toml_path)
{
    auto data = toml::parse(toml_path);
    //default kalman filter params
    key_ = toml::find<int>(data, "key");

    if (key_==0)
    {
        auto& ekf = toml::find(data, "EKF");

        std::vector<double> tmp = toml::find<std::vector<double>>(ekf, "init_state_std");
        Vector12d init_state_std = Eigen::Map<Eigen::VectorXd, Eigen::Unaligned>(tmp.data(), tmp.size());
        init_state_std = init_state_std.array().square();
        P_ = init_state_std.asDiagonal();

        tmp = toml::find<std::vector<double>>(ekf, "process_noise");
        Vector6d process_noise = Eigen::Map<Eigen::VectorXd, Eigen::Unaligned>(tmp.data(), tmp.size());
        process_noise = process_noise.array().square();
        process_cov_ = process_noise.asDiagonal();

        tmp = toml::find<std::vector<double>>(ekf, "measurement_noise");
        Vector6d measurement_noise = Eigen::Map<Eigen::VectorXd, Eigen::Unaligned>(tmp.data(), tmp.size());
        measurement_noise = measurement_noise.array().square();
        N_ = measurement_noise.asDiagonal();

        sigma_ << 1.0, 1.0, 1.0, 3.14, 3.14, 3.14;
        sigma_ = sigma_ * dt_;
    } 
    else if (key_ == 1)
    {
        //innovation saturated kalman filter params
        auto& isekf = toml::find(data, "ISEKF");

        std::vector<double> tmp = toml::find<std::vector<double>>(isekf, "init_state_std");
        Vector12d init_state_std = Eigen::Map<Eigen::VectorXd, Eigen::Unaligned>(tmp.data(), tmp.size());
        init_state_std = init_state_std.array().square();
        P_ = init_state_std.asDiagonal();

        tmp = toml::find<std::vector<double>>(isekf, "process_noise");
        Vector6d process_noise = Eigen::Map<Eigen::VectorXd, Eigen::Unaligned>(tmp.data(), tmp.size());
        process_noise = process_noise.array().square();
        process_cov_ = process_noise.asDiagonal();

        tmp = toml::find<std::vector<double>>(isekf, "measurement_noise");
        Vector6d measurement_noise = Eigen::Map<Eigen::VectorXd, Eigen::Unaligned>(tmp.data(), tmp.size());
        measurement_noise = measurement_noise.array().square();
        N_ = measurement_noise.asDiagonal();

        tmp = toml::find<std::vector<double>>(isekf, "sigma_init");
        sigma_init_ = Eigen::Map<Eigen::VectorXd, Eigen::Unaligned>(tmp.data(), tmp.size());
        sigma_init_ = sigma_init_ * dt_ * dt_;

        tmp = toml::find<std::vector<double>>(isekf, "epsilon_init");
        epsilon_init_ = Eigen::Map<Eigen::VectorXd, Eigen::Unaligned>(tmp.data(), tmp.size());

        // tmp = toml::find<std::vector<double>>(isekf, "lambda1");
        // lambda1_ = Eigen::Map<Eigen::VectorXd, Eigen::Unaligned>(tmp.data(), tmp.size());
        double tmp1 = toml::find<double>(isekf, "lambda1");
        lambda1_ << tmp1, tmp1, tmp1, tmp1, tmp1, tmp1;

        tmp1 = toml::find<double>(isekf, "lambda2");
        lambda2_ << tmp1, tmp1, tmp1, tmp1, tmp1, tmp1;

        tmp1 = toml::find<double>(isekf, "gamma1");
        gamma1_ << tmp1, tmp1, tmp1, tmp1, tmp1, tmp1;

        tmp1 = toml::find<double>(isekf, "gamma2");
        gamma2_ << tmp1, tmp1, tmp1, tmp1, tmp1, tmp1;
    }
}


}
