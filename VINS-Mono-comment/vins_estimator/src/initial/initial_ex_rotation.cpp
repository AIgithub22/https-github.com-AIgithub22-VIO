
/* This class help you to calibrate extrinsic rotation between imu and camera when your totally don't konw the extrinsic parameter */
#include "initial_ex_rotation.h"

InitialEXRotation::InitialEXRotation(){
    frame_count = 0;
    Rc.push_back(Matrix3d::Identity());
    Rc_g.push_back(Matrix3d::Identity());
    Rimu.push_back(Matrix3d::Identity());
    ric = Matrix3d::Identity();
}
// corres [匹配点]的数据组织在estimator.cpp corres = f_manager.getCorresponding(i, WINDOW_SIZE);在滑窗中寻找与最新的关键帧共视关系较强的关键帧]
//！在滑窗内寻找与最新的关键帧共视点超过20(像素点)的关键帧
/**
 * [InitialEXRotation::CalibrationExRotation 通过一组匹配点和IMU的预积分结果，计算相机与IMU的外参的旋转量
 *                           这部分内容可以参考代码总结中3.1部分，相机与IMU的相对旋转求取
 *                           或者参考论文Monocular Visual–Inertial State Estimation With Online Initialization and Camera–IMU Extrinsic Calibration
 *                           V.A 部分]
 * @param  corres           [一组匹配的特征点]
 * @param  delta_q_imu      k ==> k+1
 * @param  calib_ric_result [Camera与IMU的外参之旋转量]
 * @return                  [description]
 */
bool InitialEXRotation::CalibrationExRotation(vector<pair<Vector3d, Vector3d>> corres, Quaterniond delta_q_imu, Matrix3d &calib_ric_result)
{
    //! Step1：滑窗内的帧数加1
    frame_count++;

    //! Step2:计算两帧之间的旋转矩阵
    Rc.push_back(solveRelativeR(corres));//纯摄像头的转动矩阵
    Rimu.push_back(delta_q_imu.toRotationMatrix());//纯IMU的转动矩阵
    Rc_g.push_back(ric.inverse() * delta_q_imu * ric);

    Eigen::MatrixXd A(frame_count * 4, 4);
    A.setZero();
    int sum_ok = 0;
    for (int i = 1; i <= frame_count; i++)
    {
        Quaterniond r1(Rc[i]);
        Quaterniond r2(Rc_g[i]);

        //！Step3:求取估计出的相机与IMU之间旋转的残差
        //！r = acos((tr(R_cbRbR_bcR_c)-1)/2)
        double angular_distance = 180 / M_PI * r1.angularDistance(r2);
        ROS_DEBUG("%d %f", i, angular_distance);
        //! Step4:计算外点剔除的权重
        double huber = angular_distance > 5.0 ? 5.0 / angular_distance : 1.0;
        ++sum_ok;
        Matrix4d L, R;

        //! Step5：求取系数矩阵
        //! 得到相机对极关系得到的旋转q的左乘
        double w = Quaterniond(Rc[i]).w();
        Vector3d q = Quaterniond(Rc[i]).vec();
        L.block<3, 3>(0, 0) = w * Matrix3d::Identity() + Utility::skewSymmetric(q);
        L.block<3, 1>(0, 3) = q;
        L.block<1, 3>(3, 0) = -q.transpose();
        L(3, 3) = w;

        //! 得到由IMU预积分得到的旋转q的右乘
        Quaterniond R_ij(Rimu[i]);
        w = R_ij.w();
        q = R_ij.vec();
        R.block<3, 3>(0, 0) = w * Matrix3d::Identity() - Utility::skewSymmetric(q);
        R.block<3, 1>(0, 3) = q;
        R.block<1, 3>(3, 0) = -q.transpose();
        R(3, 3) = w;

        A.block<4, 4>((i - 1) * 4, 0) = huber * (L - R);
    }

    //！Step6：通过SVD分解，求取相机与IMU的相对旋转
    //！解为系数矩阵A的右奇异向量V中最小奇异值对应的特征向量
    JacobiSVD<MatrixXd> svd(A, ComputeFullU | ComputeFullV);
    Matrix<double, 4, 1> x = svd.matrixV().col(3);
    Quaterniond estimated_R(x);
    ric = estimated_R.toRotationMatrix().inverse();
    //cout << svd.singularValues().transpose() << endl;
    //cout << ric << endl;
    //！Step7：判断对于相机与IMU的相对旋转是否满足终止条件
    //！1.用来求解相对旋转的IMU-Cmaera的测量对数是否大于滑窗大小。
    //！2.A矩阵第二小的奇异值是否大于某个阈值，使A得零空间的秩为1
    Vector3d ric_cov;
    ric_cov = svd.singularValues().tail<3>();
    if (frame_count >= WINDOW_SIZE && ric_cov(1) > 0.25)
    {
        calib_ric_result = ric;
        return true;
    }
    else
        return false;
}

/**
 * [InitialEXRotation::solveRelativeR 通过一组匹配点求解出两帧之间的旋转, 求解出的是前帧到后帧的变换]
 * @param  corres [匹配点]
 * @return        [description]
 */
Matrix3d InitialEXRotation::solveRelativeR(const vector<pair<Vector3d, Vector3d>> &corres)
{
    //！确保匹配点的数目大于9
    if (corres.size() >= 9)
    {
        //! Step1:提取匹配点
        vector<cv::Point2f> ll, rr;
        for (int i = 0; i < int(corres.size()); i++)
        {
            ll.push_back(cv::Point2f(corres[i].first(0), corres[i].first(1)));
            rr.push_back(cv::Point2f(corres[i].second(0), corres[i].second(1)));
        }

        //！Step2：根据匹配点求取本质矩阵E
        cv::Mat E = cv::findFundamentalMat(ll, rr);
        cv::Mat_<double> R1, R2, t1, t2;
        //！Step3：分解本质矩阵E
        decomposeE(E, R1, R2, t1, t2);

        //！要确保本质矩阵E的行列式值为1的约束
        if (determinant(R1) + 1.0 < 1e-09)
        {
            E = -E;
            decomposeE(E, R1, R2, t1, t2);
        }

        //! 通过三角化选择最合适的R|t
        double ratio1 = max(testTriangulation(ll, rr, R1, t1), testTriangulation(ll, rr, R1, t2));
        double ratio2 = max(testTriangulation(ll, rr, R2, t1), testTriangulation(ll, rr, R2, t2));
        cv::Mat_<double> ans_R_cv = ratio1 > ratio2 ? R1 : R2;

        //! 这个地方对旋转矩阵做了一次转置
        Matrix3d ans_R_eigen;
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++)
                ans_R_eigen(j, i) = ans_R_cv(i, j);

        return ans_R_eigen;
    }
    return Matrix3d::Identity();
}

/**
 * [InitialEXRotation::testTriangulation 计算由三角化恢复之后的3D点满足正确重构关系的比例
 *                         可以看做是对该R|t的一个评分]
 * @param  l [description]
 * @param  r [description]
 * @param  R [description]
 * @param  t [description]
 * @return   [description]
 */
double InitialEXRotation::testTriangulation(const vector<cv::Point2f> &l,
                                          const vector<cv::Point2f> &r,
                                          cv::Mat_<double> R, cv::Mat_<double> t)
{
    cv::Mat pointcloud;
    //! 相机矩阵
    cv::Matx34f P = cv::Matx34f(1, 0, 0, 0,
                                0, 1, 0, 0,
                                0, 0, 1, 0);
    //! 变换矩阵
    cv::Matx34f P1 = cv::Matx34f(R(0, 0), R(0, 1), R(0, 2), t(0),
                                 R(1, 0), R(1, 1), R(1, 2), t(1),
                                 R(2, 0), R(2, 1), R(2, 2), t(2));

    //! 三角化恢复出3D点，三角化之后的点在l下
    cv::triangulatePoints(P, P1, l, r, pointcloud);
    int front_count = 0;

    //! 转到前一帧的坐标系下，统计3D点出现在两个摄像机前面的次数
    for (int i = 0; i < pointcloud.cols; i++)
    {
        double normal_factor = pointcloud.col(i).at<float>(3);

        //! 转到前一帧的坐标系下
        cv::Mat_<double> p_3d_l = cv::Mat(P) * (pointcloud.col(i) / normal_factor);
        cv::Mat_<double> p_3d_r = cv::Mat(P1) * (pointcloud.col(i) / normal_factor);
        //! 验证重构点是否同时在两个摄像机的前面
        if (p_3d_l(2) > 0 && p_3d_r(2) > 0)
            front_count++;
    }
    ROS_DEBUG("MotionEstimator: %f", 1.0 * front_count / pointcloud.cols);
    //！返回恢复出的3D点中满足正常重构条件的比例
    return 1.0 * front_count / pointcloud.cols;
}

/**
 * [InitialEXRotation::decomposeE 分解本质矩阵E
 *           参考中文版多视图几何P174]
 * @param E  [description]
 * @param R1 [description]
 * @param R2 [description]
 * @param t1 [description]
 * @param t2 [description]
 */
void InitialEXRotation::decomposeE(cv::Mat E,
                                 cv::Mat_<double> &R1, cv::Mat_<double> &R2,
                                 cv::Mat_<double> &t1, cv::Mat_<double> &t2)
{
    cv::SVD svd(E, cv::SVD::MODIFY_A);
    cv::Matx33d W(0, -1, 0,
                  1, 0, 0,
                  0, 0, 1);
    cv::Matx33d Wt(0, 1, 0,
                   -1, 0, 0,
                   0, 0, 1);
    R1 = svd.u * cv::Mat(W) * svd.vt;
    R2 = svd.u * cv::Mat(Wt) * svd.vt;
    t1 = svd.u.col(2);
    t2 = -svd.u.col(2);
}
