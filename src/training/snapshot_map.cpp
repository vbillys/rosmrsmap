/*
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2013, University of Bonn, Computer Science Institute VI
 *  Author: Joerg Stueckler, 13.06.2013
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of University of Bonn, Computer Science Institute
 *     VI nor the names of its contributors may be used to endorse or
 *     promote products derived from this software without specific
 *     prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *
 */


#include <ros/ros.h>
#include <ros/package.h>

#include <pcl_ros/point_cloud.h>
#include <pcl/point_types.h>
#include <pcl/ros/conversions.h>
#include <pcl_ros/publisher.h>
#include <pcl_ros/transforms.h>

#include <rosbag/bag.h>
#include <rosbag/view.h>
#include <rosbag/query.h>
#include <rosbag/message_instance.h>
#include <boost/foreach.hpp>
#define foreach BOOST_FOREACH

#include <image_transport/image_transport.h>
#include <cv_bridge/cv_bridge.h>
#include <opencv2/highgui/highgui.hpp>
#include <image_geometry/pinhole_camera_model.h>

#include <sensor_msgs/Image.h>
#include <sensor_msgs/CameraInfo.h>
#include <std_msgs/Float32.h>
#include <geometry_msgs/PoseStamped.h>
//#include <rosmrsmap/ObjectData.h>
#include <sensor_msgs/image_encodings.h>
#include <sensor_msgs/distortion_models.h>

#include <geometry_msgs/PoseArray.h>
#include <visualization_msgs/MarkerArray.h>
//#include <rosmrsmap/SelectBoundingBox.h>
//#include <rosmrsmap/TriggerInitialAlignment.h>

#include <Eigen/Core>

#include <tf/transform_broadcaster.h>

#include <pcl/segmentation/extract_polygonal_prism_data.h>
#include <pcl/surface/convex_hull.h>

#include <mrsmap/map/multiresolution_surfel_map.h>
#include <mrsmap/registration/multiresolution_surfel_registration.h>


#include <boost/thread/thread.hpp>
#include "pcl/common/common_headers.h"
#include "pcl/visualization/pcl_visualizer.h"


#include "pcl/common/centroid.h"
#include "pcl/common/eigen.h"


#include <rosmrsmap/StringService.h>
#include <std_msgs/Int32.h>


using namespace mrsmap;



class SnapshotMap
{
public:

    SnapshotMap( ros::NodeHandle& nh ) : nh_( nh ) {

		imageAllocator_ = boost::shared_ptr< MultiResolutionSurfelMap::ImagePreAllocator >( new MultiResolutionSurfelMap::ImagePreAllocator() );
		treeNodeAllocator_ = boost::shared_ptr< spatialaggregate::OcTreeNodeDynamicAllocator< float, MultiResolutionSurfelMap::NodeValue > >( new spatialaggregate::OcTreeNodeDynamicAllocator< float, MultiResolutionSurfelMap::NodeValue >( 10000 ) );

		snapshot_service_ = nh.advertiseService("snapshot", &SnapshotMap::snapshotRequest, this);
		pub_cloud = pcl_ros::Publisher<pcl::PointXYZRGB>(nh, "output_cloud", 1);

		pub_status_ = nh.advertise< std_msgs::Int32 >( "status", 1 );

		tf_listener_ = boost::shared_ptr< tf::TransformListener >( new tf::TransformListener() );

		nh.param<double>( "max_resolution", max_resolution_, 0.0125 );
		nh.param<double>( "max_radius", max_radius_, 30.0 );

		nh.param<std::string>( "map_folder", map_folder_, "." );

		create_map_ = false;

		responseId_ = -1;

    }


	bool snapshotRequest( rosmrsmap::StringService::Request &req, rosmrsmap::StringService::Response &res ) {

		object_name_ = req.str;
		sub_cloud_ = nh_.subscribe( "input_cloud", 1, &SnapshotMap::dataCallback, this );
		create_map_ = true;

		ROS_INFO_STREAM( "subscribed at " << sub_cloud_.getTopic() );

		res.responseId = responseId_ + 1;

		return true;

	}


	void dataCallback(const sensor_msgs::PointCloud2ConstPtr& point_cloud) {

		if( !create_map_ )
			return;

		ROS_INFO("creating map");


		pcl::PointCloud<pcl::PointXYZRGB>::Ptr pointCloudIn = pcl::PointCloud<pcl::PointXYZRGB>::Ptr( new pcl::PointCloud<pcl::PointXYZRGB>() );
		pcl::fromROSMsg(*point_cloud, *pointCloudIn);

		pointCloudIn->sensor_orientation_.setIdentity();
		pointCloudIn->sensor_origin_.setZero();
		pointCloudIn->sensor_origin_(3) = 1.0;

		treeNodeAllocator_->reset();
		boost::shared_ptr< MultiResolutionSurfelMap > map = boost::shared_ptr< MultiResolutionSurfelMap >( new MultiResolutionSurfelMap( max_resolution_, max_radius_, treeNodeAllocator_ ) );

		std::vector< int > pointIndices( pointCloudIn->points.size() );
		for( unsigned int i = 0; i < pointIndices.size(); i++ ) pointIndices[i] = i;
		std::vector< int > imageBorderIndices;
		map->imageAllocator_ = imageAllocator_;
		map->addPoints( *pointCloudIn, pointIndices );
		map->octree_->root_->establishNeighbors();
		map->markNoUpdateAtPoints( *pointCloudIn, imageBorderIndices );
		map->evaluateSurfels();
		map->buildShapeTextureFeatures();

		map->save( map_folder_ + "/" + object_name_ + ".map" );

		pcl::PointCloud< pcl::PointXYZRGB >::Ptr cloudv = pcl::PointCloud< pcl::PointXYZRGB >::Ptr( new pcl::PointCloud< pcl::PointXYZRGB >() );
		cloudv->header = pointCloudIn->header;
		map->visualize3DColorDistribution( cloudv, -1, -1, false );
		pub_cloud.publish( cloudv );


		sub_cloud_.shutdown();

		create_map_ = false;

		responseId_++;

	}

	void update() {

		std_msgs::Int32 status;
		status.data = responseId_;
		pub_status_.publish( status );

	}


public:

	ros::NodeHandle nh_;
	ros::Subscriber sub_cloud_;
	ros::Publisher pub_status_;
	pcl_ros::Publisher<pcl::PointXYZRGB> pub_cloud;
	boost::shared_ptr< tf::TransformListener > tf_listener_;

	double max_resolution_, max_radius_;

	bool create_map_;

	ros::ServiceServer snapshot_service_;

	std::string map_folder_;
	std::string object_name_;

	boost::shared_ptr< MultiResolutionSurfelMap::ImagePreAllocator > imageAllocator_;
	boost::shared_ptr< spatialaggregate::OcTreeNodeDynamicAllocator< float, MultiResolutionSurfelMap::NodeValue > > treeNodeAllocator_;

	int responseId_;

};



int main(int argc, char** argv) {

	ros::init(argc, argv, "snapshot_map");
	ros::NodeHandle n("snapshot_map");
	SnapshotMap sm( n );

	ros::Rate r( 30 );
	while( ros::ok() ) {

		sm.update();

		ros::spinOnce();
		r.sleep();

	}


	return 0;
}

