/*************************************************************************

WASS - Wave Acquisition Stereo System
Copyright (C) 2016  Filippo Bergamasco

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.

*************************************************************************/



#include <opencv2/core.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/calib3d.hpp>
#include <fstream>

#include "wassglobal.hpp"
#include "log.hpp"
#include "utils.hpp"
#include "incfg.hpp"
#include "triangulate.hpp"
#include "epipolar.h"
#include "sba_driver.h"


static boost::filesystem::path workdir;
using WASS::epi::ErrorStats;



int main( int argc, char* argv[] )
{
    std::vector< boost::filesystem::path > workspaces;
    cv::Mat K0;
    cv::Mat K1;
    cv::Matx33d K0i;
    cv::Matx33d K1i;
    std::vector< cv::Point2d > pts_0;
    std::vector< cv::Point2d > pts_1;
    std::vector< cv::Point2d > pts_0_n;
    std::vector< cv::Point2d > pts_1_n;

    WASS::exe_name_to_stdout( "wass_autocalibrate" );

    if( argc == 1 )
    {
        std::cout << "Usage:" << std::endl;
        std::cout << "wass_autocalibrate <workdirs_file>" << std::endl << std::endl;
        std::cout << "Not enough arguments, aborting." << std::endl;
        return -1;
    }

    if( argc != 2 )
    {
        std::cerr << "Invalid arguments" << std::endl;
        return -1;
    }

    WASS::setup_logger();
    LOG_SCOPE("wass_autocalibrate");

    {
        LOGI << "Loading workspaces...";
        std::ifstream ifs( argv[1] );
        if( !ifs.is_open() )
        {
            LOGE << "Unable to load " << argv[2];
            return -1;
        }

        while( !ifs.fail() && !ifs.eof() )
        {
            std::string ws;
            ifs >> ws;
            boost::filesystem::path pws(ws);
            if( boost::filesystem::exists(pws) && boost::filesystem::is_directory(pws) )
                workspaces.push_back( pws );
        }

        LOGI << workspaces.size() << " workspace directories loaded";
    }

    try
    {
        LOGI << "Loading matches";

        std::cout << "[P|20|100]" << std::endl;

        for( std::vector< boost::filesystem::path >::const_iterator it=workspaces.begin(); it != workspaces.end(); ++it )
        {
            // Load intrinsic parameters if needed
            if( K0.rows != 3 || K0.cols != 3 || K1.rows != 3 || K1.cols != 3 )
            {
                K0 = WASS::load_matrix( *it / "intrinsics_00000000.xml" );
                K1 = WASS::load_matrix( *it / "intrinsics_00000001.xml" );
                K0i = cv::Matx33d((double*)(K0.clone().ptr())); K0i = K0i.inv();
                K1i = cv::Matx33d((double*)(K1.clone().ptr())); K1i = K1i.inv();
            }

            // Load matches
            std::ifstream ifs( ((*it)/"matches.txt").string().c_str() );
            if( !ifs.is_open() )
            {
                LOGE << "Unable to load matches from " << (*it)/"matches.txt" << ", skipping";
                continue;
            }
            size_t n_matches;
            ifs >> n_matches;

            for( size_t k=0; k<n_matches; ++k )
            {
                cv::Vec2d p0;
                cv::Vec2d p1;
                ifs >> p0[0];
                ifs >> p0[1];
                ifs >> p1[0];
                ifs >> p1[1];

                pts_0.push_back( cv::Point2d(p0[0],p0[1]) );
                pts_1.push_back( cv::Point2d(p1[0],p1[1]) );

                cv::Vec3d p0n = K0i*cv::Vec3d(p0[0],p0[1],1);
                cv::Vec3d p1n = K1i*cv::Vec3d(p1[0],p1[1],1);

                pts_0_n.push_back( cv::Point2d(p0n[0],p0n[1]) );
                pts_1_n.push_back( cv::Point2d(p1n[0],p1n[1]) );
            }

            LOGI << n_matches << " matches loaded";
        }

        std::cout << "[P|50|100]" << std::endl;
        LOGI << pts_0.size() << " total matches loaded.";

        LOGI << "Estimating global essential matrix";

        cv::Mat mask;
        cv::Mat E = cv::findEssentialMat( pts_0_n, pts_1_n, 1.0, cv::Point2d(0,0), cv::LMEDS, 0.99, /*2px max distance*/ 2.0/K0.at<double>(0,0), mask );

        cv::Mat R;
        cv::Mat T;
        cv::recoverPose( E, pts_0_n, pts_1_n, R, T, 1.0, cv::Point2d(0,0), mask );

        LOGI << cv::sum(mask)[0] << " inliers";

        std::vector< cv::Vec2d > pts0;
        std::vector< cv::Vec2d > pts1;
        std::vector< cv::Vec2d > pts0_px;
        std::vector< cv::Vec2d > pts1_px;
        std::vector< cv::Vec3d > pts3d;

        for( size_t i=0; i<mask.rows; ++i )
        {
            if( mask.at<bool>(i) )
            {
                pts0.push_back( cv::Vec2d( pts_0_n[i].x, pts_0_n[i].y ) );
                pts1.push_back( cv::Vec2d( pts_1_n[i].x, pts_1_n[i].y ) );
                pts0_px.push_back( cv::Vec2d( pts_0[i].x, pts_0[i].y ) );
                pts1_px.push_back( cv::Vec2d( pts_1[i].x, pts_1[i].y ) );
                cv::Vec3d pt3d = triangulate( pts0.back(), pts1.back(), R, T );
                pts3d.push_back( pt3d );

                if( pt3d[2] < 0 )
                {
                    // Negative depth
                    LOGE << "An estimated 3D points is behind the stereo rig.";
                    return -1;
                }
            }
        }

        pts_0_n.clear(); pts_1_n.clear();
        pts_0.clear(); pts_1.clear();

        cv::Matx33d Ex((double*)(E.clone().ptr()));
        cv::Matx33d F = K1i.t() * Ex * K0i;

        ErrorStats er = WASS::epi::evaluate_structure_error( pts3d, pts0_px, pts1_px, R, T, K0, K1 );
        LOGI << "Structure reprojection error: " << er.avg <<  "+-" << er.std << " px. Min: " << er.min << " Max: " << er.max;

        er = WASS::epi::evaluate_epipolar_error( F, pts0_px, pts1_px );
        double ransac_avgerr = er.avg;
        LOGI << "Epipolar error: " << er.avg <<  "+-" << er.std << " px. Min: " << er.min << " Max: " << er.max;


        std::cout << "[P|70|100]" << std::endl;

        // Create SBA structures

        std::vector< cv::Matx44d > cam_poses;

        cam_poses.push_back( cv::Matx44d::eye() );
        cv::Matx44d cam1pose = cv::Matx44d::eye();
        for( int i=0; i<3; ++i )
            for( int j=0; j<3; ++j )
                cam1pose(i,j) = R.at<double>(i,j);
        cam1pose(0,3) = T.at<double>(0,0);
        cam1pose(1,3) = T.at<double>(1,0);
        cam1pose(2,3) = T.at<double>(2,0);
        cam_poses.push_back( cam1pose );

        std::vector< std::vector< cv::Vec2d > > points_rep;
        points_rep.push_back( pts0 );
        points_rep.push_back( pts1 );

        std::vector< cv::Mat > sbaR;
        std::vector< cv::Mat > sbaT;

        // run sba

        sba_driver( cam_poses, pts3d, points_rep, sbaR, sbaT );

        cv::Mat R1i = sbaR[0].t();
        cv::Mat T1i = -R1i * sbaT[0];

        R = sbaR[1]*R1i;
        T = sbaR[1]*T1i + sbaT[1];
        T = T/cv::norm(T);


        // Recompute epipolar error
        cv::Mat Tx = cv::Mat::zeros(3,3,CV_64FC1);
        Tx.at<double>(0,1) = -T.at<double>(2,0);
        Tx.at<double>(0,2) =  T.at<double>(1,0);
        Tx.at<double>(1,0) =  T.at<double>(2,0);
        Tx.at<double>(1,2) = -T.at<double>(0,0);
        Tx.at<double>(2,0) = -T.at<double>(1,0);
        Tx.at<double>(2,1) =  T.at<double>(0,0);

        E = Tx*R;
        Ex = cv::Matx33d((double*)(E.clone().ptr()));
        F = K1i.t() * Ex * K0i;

        er = WASS::epi::evaluate_epipolar_error( F, pts0_px, pts1_px );
        LOGI << "SBA-optimized epipolar error: " << er.avg <<  "+-" << er.std << " px. Min: " << er.min << " Max: " << er.max;

        if( er.avg < ransac_avgerr )
        {
            // Save update extrinsics to each workspace
            for( std::vector< boost::filesystem::path >::const_iterator it=workspaces.begin(); it != workspaces.end(); ++it )
            {
                cv::FileStorage fs( ((*it)/"ext_R.xml").string(), cv::FileStorage::WRITE );
                fs << "ext_R" << R;
                fs.release();
                fs.open( ((*it)/"ext_T.xml").string(), cv::FileStorage::WRITE );
                fs << "ext_T" << T;
                fs.release();
            }
        } else
        {
            LOGE << "SBA failed. reverting to ransac calibration";
        }

        LOGI << "All done!";

        std::cout << "[P|100|100]" << std::endl;

    } catch( std::runtime_error& ex )
    {
        LOGE << ex.what();
        return -1;
    }

    return 0;
}
