// SPDX-FileCopyrightText: 2023 MakotoYoshigoe
// SPDX-License-Identifier: Apache-2.0

#include "gnss2map/GaussKruger.hpp"
#include <cmath>
#include <vector>
#include <array>

namespace gnss2map
{
    GaussKruger::GaussKruger() : Node("gauss_kruger")
    {
        setParam();
        getParam();
        initPubSub();
        initVariable();
    }

    GaussKruger::~GaussKruger(){}

    void GaussKruger::setParam()
    {
        this->declare_parameter("p0", std::vector<double>(3, 0.0));
        this->declare_parameter("gnss0", std::vector<double>(3, 0.0));
        this->declare_parameter("p1", std::vector<double>(2, 0.0));
        this->declare_parameter("gnss1", std::vector<double>(2, 0.0));
        this->declare_parameter("a", 6378137.0);
        this->declare_parameter("F", 298.257222);
        this->declare_parameter("m0", 0.9999);
        this->declare_parameter("ignore_th_cov", 16.0);
        // this->declare_parameter("range_limit", std::vector<double>(4, 0.0));
    }

    void GaussKruger::getParam()
    {
        this->get_parameter("p0", p0_);
        this->get_parameter("gnss0", gnss0_);
        this->get_parameter("p1", p1_);
        this->get_parameter("gnss1", gnss1_);
        this->get_parameter("a", a_);
        this->get_parameter("F", F_);
        this->get_parameter("m0", m0_);
        this->get_parameter("ignore_th_cov", ignore_th_cov_);
        // this->get_parameter("range_limit", range_limit_);
    }

    void GaussKruger::initPubSub()
    {
        sub_gnss_ = this->create_subscription<sensor_msgs::msg::NavSatFix>("gnss/fix", 2, std::bind(&GaussKruger::cbGnss, this, std::placeholders::_1));
        // pub_odom_gnss_ = this->create_publisher<nav_msgs::msg::Odometry>("odom/gnss", 2);
        pub_gnss_pose_ = this->create_publisher<geometry_msgs::msg::PoseWithCovarianceStamped>("gnss_pose", 2);
    }

    void GaussKruger::cbGnss(sensor_msgs::msg::NavSatFix::ConstSharedPtr msg)
    {
        double covariance = msg->position_covariance[0];
        std::array<double, 9UL> cov = msg->position_covariance;
        // RCLCPP_INFO(this->get_logger(), "cov (xx, yy): (%lf, %lf)", cov[0], cov[4]);
        int8_t status = msg->status.status;
        double x, y, z = msg->altitude + offset_z_;
        if(covariance > ignore_th_cov_ || status == NO_FIX){
            x = NAN, y = NAN, z = NAN;
        } else {
            double rad_phi = msg->latitude*M_PI/180;
            double rad_lambda = msg->longitude*M_PI/180;
            gaussKruger(rad_phi, rad_lambda, x, y);
            // if(outOfRange(x, y)){
            //     RCLCPP_INFO(this->get_logger(), "Out");
            //     x = NAN;
            //     y = NAN;
            // }
        }
        // pubOdomGnss(x, y, z);
        pubGnssPose(x, y, z, cov[0], cov[4], cov[8]);
    }

    void GaussKruger::initVariable()
    {
        for(int i=0; i<2; ++i){
            gnss0_[i] *= M_PI/180;
            gnss1_[i] *= M_PI/180;
        }
        offset_z_ = p0_[2] - gnss0_[2];
        double n = 1 / (2 * F_ - 1);
        double n2 = pow(n, 2), n3 = pow(n, 3), n4 = pow(n, 4), n5 = pow(n, 5);
        alpha_[0] = n/2 - 2.*n2/3 + 5.*n3/16 + 41.*n4/180 - 127.*n5/288;
        alpha_[1] = 13.*n2/48 - 3.*n3/5 + 557.*n4/1440 + 281.*n5/630;
        alpha_[2] = 61.*n3/240 - 103.*n4/140 + 15061.*n5/26880;
        alpha_[3] = 49561.*n4/161280 - 179.*n5/168;
        alpha_[4] = 34729.*n5/80640;

        A_[0] = 1. + n2/4 + n4/64;
        A_[1] = -3./2*(n - n3/8 - n5/64);
        A_[2] = 15./16*(n2 - n4/4);
        A_[3] = -35./48*(n3 - 5.*n5/16);
        A_[4] = 315.*n4/512;
        A_[5] = -693.*n5/1280;

        A_bar_ = m0_ * a_ * A_[0]/(1 + n);

        S_bar_phi0_ = A_[0]*gnss0_[0];
        for(int i=1; i<=5; ++i) S_bar_phi0_ += A_[i]*sin(2*i*gnss0_[0]);
        S_bar_phi0_ *= m0_*a_ /(1+n);

        kt_ = 2*sqrt(n) / (1+n);

        K_ << 1., 0., 0., 1.;
        double rad_theta_offset_ = 0.0;
        R_ = Eigen::Rotation2Dd(rad_theta_offset_);

        double x0, y0, x1, y1;
        gaussKruger(gnss0_[0], gnss0_[1], x0, y0);
        gaussKruger(gnss1_[0], gnss1_[1], x1, y1);
        rad_theta_offset_ = atan2(y1-y0, x1-x0) - atan2(p1_[1]-p0_[1], p1_[0]-p0_[0]);
        R_ = Eigen::Rotation2Dd(rad_theta_offset_);

        gaussKruger(gnss1_[0], gnss1_[1], x1, y1);
        K_ << p1_[0] / x1, 0., 0., p1_[1] / y1;
        RCLCPP_INFO(this->get_logger(), "kx: %lf, ky: %lf, theta: %lf", K_(0, 0), K_(1, 1), R_.angle());
    }

    void GaussKruger::gaussKruger(double rad_phi, double rad_lambda, double &x, double &y)
    {
        double t = sinh(atanh(sin(rad_phi)) - kt_*atanh(kt_*sin(rad_phi)));
        double t_bar = sqrt(1+pow(t, 2));
        double diff_lambda = rad_lambda - gnss0_[1];
        double lambda_cos = cos(diff_lambda), lambda_sin = sin(diff_lambda);
        double zeta = atan2(t, lambda_cos), eta = atanh(lambda_sin / t_bar);
        Eigen::Vector2d p;
        p(0) = zeta;
        p(1) = eta;
        for(int i=1; i<=5; ++i){
            p(0) += alpha_[i-1] * sin(2*i*zeta) * cosh(2*i*eta);
            p(1) += alpha_[i-1] * cos(2*i*zeta) * sinh(2*i*eta);
        }
        p(0) = p(0) * A_bar_ - S_bar_phi0_;
        p(1) *= A_bar_;
        p = K_ * R_ * p;
        x = p(0) + p0_[0];
        y = -p(1) + p0_[1];
    }

    // void GaussKruger::pubOdomGnss(double x, double y, double z)
    // {
    //     nav_msgs::msg::Odometry odom;
    //     odom.header.stamp = now();
    //     odom.header.frame_id = "map";
    //     odom.child_frame_id = "base_footprint";
    //     odom.pose.pose.position.x = x;
    //     odom.pose.pose.position.y = y;
    //     odom.pose.pose.position.z = z;
    //     pub_odom_gnss_->publish(odom);
    // }

    void GaussKruger::pubGnssPose(double x, double y, double z, double dev_x, double dev_y, double dev_z)
    {
        geometry_msgs::msg::PoseWithCovarianceStamped pose;
        pose.header.frame_id = "map";
        pose.header.stamp = now();
        pose.pose.pose.position.x = x;
        pose.pose.pose.position.y = y;
        pose.pose.pose.position.z = z;
        pose.pose.covariance[0] = dev_x;
	    pose.pose.covariance[7] = dev_y;
        pose.pose.covariance[14] = dev_z;
        pub_gnss_pose_->publish(pose);
    }

    // bool GaussKruger::outOfRange(double x, double y)
    // {
    //     return (x < range_limit_[0]  || y < range_limit_[1] || x >= range_limit_[2] || y >= range_limit_[3]);
    // }
}

int main(int argc, char ** argv)
{
    rclcpp::init(argc, argv);
    auto node = std::make_shared<gnss2map::GaussKruger>();
    rclcpp::spin(node);
    rclcpp::shutdown();
    return 0;
}
