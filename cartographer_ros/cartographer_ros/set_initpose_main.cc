/*
 * Copyright 2019 The Cartographer Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


#include "gflags/gflags.h"
#include "cartographer/common/configuration_file_resolver.h"
#include "cartographer/io/proto_stream.h"
#include "cartographer/mapping/map_builder.h"
#include "cartographer_ros/node_options.h"
#include "cartographer_ros/node_constants.h"
#include "cartographer_ros/ros_log_sink.h"
#include "cartographer_ros_msgs/StartTrajectory.h"
#include "cartographer_ros_msgs/FinishTrajectory.h"
#include "cartographer_ros_msgs/StatusCode.h"

#include "ros/ros.h"
#include "std_msgs/String.h"
#include "tf2_geometry_msgs/tf2_geometry_msgs.h"
#include "geometry_msgs/PoseWithCovarianceStamped.h"

#include <string>
#include <vector>

DEFINE_string(configuration_directory, "",
              "First directory in which configuration files are searched, "
              "second is always the Cartographer installation to allow "
              "including files from there.");
DEFINE_string(configuration_basename, "",
              "Basename, i.e. not containing any directory prefix, of the "
              "configuration file.");

DEFINE_string(load_state_filename, "",
              "Filename of a pbstream to draw a map from.");

namespace
{
  int current_trajectory_id_ = 1;
  std::unique_ptr<cartographer::mapping::MapBuilder> map_builder_;
}


// subscribe callback function from Rviz
void move_base_simple_callback(const geometry_msgs::PoseWithCovarianceStamped::ConstPtr& msg){
  ::ros::NodeHandle nh;

  // stop the old trajectory
  ::ros::ServiceClient client_finish_trajectroy = nh.serviceClient<cartographer_ros_msgs::FinishTrajectory>(cartographer_ros::kFinishTrajectoryServiceName);
  cartographer_ros_msgs::FinishTrajectory srv_finish_trajectroy;
  srv_finish_trajectroy.request.trajectory_id = current_trajectory_id_++;

  if (!client_finish_trajectroy.call(srv_finish_trajectroy)) {
    LOG(ERROR) << "Failed to call " << cartographer_ros::kFinishTrajectoryServiceName << ".";
  }
  if (srv_finish_trajectroy.response.status.code != cartographer_ros_msgs::StatusCode::OK) {
    LOG(ERROR) << "Error finishing trajectory - message: '"
               << srv_finish_trajectroy.response.status.message
               << "' (status code: " << std::to_string(srv_finish_trajectroy.response.status.code)
               << ").";
    return;
  }

  // start a new trajectory
  const auto node_poses = map_builder_->pose_graph()->GetTrajectoryNodePoses();

  // get the start pose of the trajectory0 w.r.t /map
  const auto traj_ref_pose = node_poses.BeginOfTrajectory(0)->data.global_pose;
  tf2::Transform traj_ref_tf;
  traj_ref_tf.getOrigin() = tf2::Vector3(traj_ref_pose.translation().x(), traj_ref_pose.translation().y(), traj_ref_pose.translation().z());
  traj_ref_tf.setRotation(tf2::Quaternion(traj_ref_pose.rotation().x(), traj_ref_pose.rotation().y(), traj_ref_pose.rotation().z(), traj_ref_pose.rotation().w()));

  // get init pose w.r.t /map
  tf2::Transform map_tf;
  tf2::fromMsg(msg->pose.pose, map_tf);

  // have to set the corret z for the initial pose, since Rviz can only assign 2D position
  const auto submap_poses = map_builder_->pose_graph()->GetAllSubmapPoses();
  double min_dist = 1e6;
  for(auto itr = submap_poses.begin(); itr != submap_poses.end(); ++itr)
    {
      // get the origin of submap w.r.t /map
      tf2::Vector3 submap_origin(itr->data.pose.translation().x(),
                                 itr->data.pose.translation().y(),
                                 itr->data.pose.translation().z());

      // calculate the distance between submap origin and initial pose
      tf2::Vector3 delta_pos = submap_origin - map_tf.getOrigin();
      delta_pos.setZ(0); // only check horinzontal location

      // find the best submap, which is closest to the initial pose
      // and assign the height (i.e. z value) of this submap to the initial pose
      if(delta_pos.length() < min_dist)
        {
          map_tf.getOrigin().setZ(submap_origin.z());
          min_dist = delta_pos.length();
        }
    }

  tf2::Transform relative_initpose_tf = traj_ref_tf.inverse() * map_tf;

  // set the start trajectory service call
  ::ros::ServiceClient client_start_trajectroy = nh.serviceClient<cartographer_ros_msgs::StartTrajectory>(cartographer_ros::kStartTrajectoryServiceName);
  cartographer_ros_msgs::StartTrajectory srv_start_trajectroy;
  srv_start_trajectroy.request.configuration_directory = FLAGS_configuration_directory;
  srv_start_trajectroy.request.configuration_basename = FLAGS_configuration_basename;

  srv_start_trajectroy.request.relative_to_trajectory_id = 0; //frozen trajectory
  srv_start_trajectroy.request.use_initial_pose = true;
  tf2::toMsg(relative_initpose_tf, srv_start_trajectroy.request.initial_pose);

  if (!client_start_trajectroy.call(srv_start_trajectroy)) {
    LOG(ERROR) << "Failed to call " << cartographer_ros::kStartTrajectoryServiceName << ".";
  }
  if (srv_start_trajectroy.response.status.code != cartographer_ros_msgs::StatusCode::OK) {
    LOG(ERROR) << "Error starting trajectory - message: '"
               << srv_start_trajectroy.response.status.message
               << "' (status code: " << std::to_string(srv_start_trajectroy.response.status.code)
               << ").";
  }
}

int main(int argc, char** argv) {
  google::InitGoogleLogging(argv[0]);

  google::SetUsageMessage(
      "\n\n"
      "Convenience tool around the start_trajectory service. This takes a Lua "
      "file that is accepted by the node as well and starts a new trajectory "
      "using its settings.\n");

  google::ParseCommandLineFlags(&argc, &argv, true);

  CHECK(!FLAGS_configuration_basename.empty())
      << "-configuration_basename is missing.";

  CHECK(!FLAGS_configuration_directory.empty())
      << "-configuration_directory is missing.";

  // load pbstream
  ::cartographer::io::ProtoStreamReader reader(FLAGS_load_state_filename);
  cartographer_ros::NodeOptions node_options;
  std::tie(node_options, std::ignore) = cartographer_ros::LoadOptions(FLAGS_configuration_directory, FLAGS_configuration_basename);
  map_builder_ =  absl::make_unique<cartographer::mapping::MapBuilder>(node_options.map_builder_options);
  map_builder_->LoadState(&reader, true);

  ::ros::init(argc, argv, "cartographer_start_trajectory");
  ::ros::start();

  ::ros::NodeHandle nh;
  ::ros::Subscriber sub_move_base_simple = nh.subscribe<geometry_msgs::PoseWithCovarianceStamped>("/initialpose", 1, &move_base_simple_callback);
  ::ros::spin();
}

