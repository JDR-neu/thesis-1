#include "drone_3d_nav/navigate.h"

namespace navigate
{
Navigator::Navigator()
{
  // Parameters
  _nh.param<double>("/x_kp", _x_kp, 0.5);
  _nh.param<double>("/x_ki", _x_ki, 0);
  _nh.param<double>("/x_kd", _x_kd, 0);
  _nh.param<double>("/y_kp", _y_kp, 0.5);
  _nh.param<double>("/y_ki", _y_ki, 0);
  _nh.param<double>("/y_kd", _y_kd, 0);
  _nh.param<double>("/z_kp", _z_kp, 0.5);
  _nh.param<double>("/z_ki", _z_ki, 0);
  _nh.param<double>("/z_kd", _z_kd, 0);
  _nh.param<double>("/yaw_kp", _yaw_kp, 0.5);
  _nh.param<double>("/yaw_ki", _yaw_ki, 0);
  _nh.param<double>("/yaw_kd", _yaw_kd, 0);

  _nh.param<float>("/max_speed/translational", _trans_max_speed, 2);
  _nh.param<float>("/max_speed/rotational", _rot_max_speed, 2);

  _must_exit = false;
  _waypoint_number = 0;
  _waypoints_received = false;
  _dt = 0;
  _hovering = false;

  _error_x = 0;
  _error_y = 0;
  _error_z = 0;
  _error_yaw = 0;

  _prev_error_x = 0;
  _prev_error_y = 0;
  _prev_error_z = 0;
  _prev_error_yaw = 0;

  _rise = 1;

  _proportional_x = 0;
  _proportional_y = 0;
  _proportional_z = 0;
  _proportional_yaw = 0;

  _integral_x = 0;
  _integral_y = 0;
  _integral_z = 0;
  _integral_yaw = 0;

  _derivative_x = 0;
  _derivative_y = 0;
  _derivative_z = 0;
  _derivative_yaw = 0;

  _action_x = 0;
  _action_y = 0;
  _action_z = 0;
  _action_yaw = 0;

  _vel_pub = _nh.advertise<geometry_msgs::Twist>("/cmd_vel", 5);
  _stamped_vel_pub = _nh.advertise<geometry_msgs::TwistStamped>("/cmd_vel/stamped", 5);
  _goal_reached_pub = _nh.advertise<std_msgs::Bool>("/goal_reached", 1);

  // amcl_pose
  _pose_sub = _nh.subscribe<geometry_msgs::PoseStamped>("/amcl_pose", 5, &Navigator::poseCallback, this);
  _waypoints_sub = _nh.subscribe<trajectory_msgs::MultiDOFJointTrajectory>("/waypoints_smooth", 1,
                                                                           &Navigator::waypointCallback, this);
}

Navigator::~Navigator()
{
}

void Navigator::waypointCallback(const trajectory_msgs::MultiDOFJointTrajectoryConstPtr& msg)
{
  // The first item in the points array is the initial position
  _waypoint_number = 1;
  _number_of_waypoints = msg->points.size();
  // Empty the queue for new waypoints
  while (!_waypoints.empty())
  {
    _waypoints.pop();
  }

  ROS_INFO("[Navigate] %d waypoints received\n", _number_of_waypoints);
  for (unsigned int i = 0; i < _number_of_waypoints; i++)
  {
    _waypoints.push(msg->points[i].transforms[0]);
  }

  _current_goal = _waypoints.front();
  _waypoints.pop();  // Remove this element from the queue
  _waypoints_received = true;
  _must_exit = false;
  _hovering = false;
  // If this is not the first time, that waypoints are sent, we need to restore tolerance value
  _nh.param<float>("/tolerance", _tolerance, 0.15);
  _nh.param<float>("yaw_tolerance", _yaw_tolerance, 0.05);
}

void Navigator::poseCallback(const geometry_msgs::PoseStampedConstPtr& msg)
{
  // If the waypoints have not received, we cannot proceed with the navigation.
  if (!_waypoints_received && !_hovering)
  {
    ROS_WARN_ONCE("[Navigate] Waypoints not received. Skipping current pose...\n");
    return;
  }

  _pose.translation.x = msg->pose.position.x;
  _pose.translation.y = msg->pose.position.y;
  _pose.translation.z = msg->pose.position.z;

  _pose.rotation = msg->pose.orientation;  // geometry_msgs::Quaternion showing the current orientation

  // Get the Yaw from current goal and odometry measurement
  tf2::Quaternion current_goal_quat, pose_quat;
  tf2::fromMsg(_current_goal.rotation, current_goal_quat);
  tf2::fromMsg(_pose.rotation, pose_quat);

  // If we want to find roll and pitch, we need Matrix3x3 and getRPY
  double current_goal_yaw = tf2::impl::getYaw(current_goal_quat);
  double pose_yaw = tf2::impl::getYaw(pose_quat);

  _dt = ros::Time::now().toSec() - _dt;

  // Find errors and PID values to calculate next action
  _error_x = _current_goal.translation.x - _pose.translation.x;
  _error_y = _current_goal.translation.y - _pose.translation.y;
  _error_z = _current_goal.translation.z - _pose.translation.z;

  _proportional_x = _x_kp * _error_x;
  _proportional_y = _y_kp * _error_y;
  _proportional_z = _z_kp * _error_z;

  _integral_x = _x_ki * (_integral_x + _error_x * _dt);
  _integral_y = _y_ki * (_integral_y + _error_y * _dt);
  _integral_z = _z_ki * (_integral_z + _error_z * _dt);

  _derivative_x = _x_kd * (_error_x - _prev_error_x) / _dt;
  _derivative_y = _y_kd * (_error_y - _prev_error_y) / _dt;
  _derivative_z = _z_kd * (_error_z - _prev_error_z) / _dt;

  _prev_error_x = _error_x;
  _prev_error_y = _error_y;
  _prev_error_z = _error_z;

  _action_x = _proportional_x + _integral_x + _derivative_x;
  _action_y = _proportional_y + _integral_y + _derivative_y;
  _action_z = _proportional_z + _integral_z + _derivative_z;

  // Clamp the velocities
  clamp(_action_x, _trans_max_speed);
  clamp(_action_y, _trans_max_speed);
  clamp(_action_z, _trans_max_speed);

  // converting from world frame to drone frame
  // Rotate with Rodriguez formula
  _action_x = _action_x * cos(pose_yaw) + _action_y * sin(pose_yaw);
  _action_y = _action_y * cos(pose_yaw) - _action_x * sin(pose_yaw);

  _twist.linear.x = _action_x;
  _twist.linear.y = _action_y;
  _twist.linear.z = _action_z;
  _twist.angular.x = 0;
  _twist.angular.y = 0;
  _twist.angular.z = 0;

  /*
    ROS_INFO("Error (X, Y, Z, Yaw) : (%0.2f, %0.2f, %0.2f, %0.2f) \n", _error_x, _error_y, _error_z, _error_yaw);
    ROS_INFO("Action (X, Y, Z, Yaw) : (%0.2f, %0.2f, %0.2f, %0.2f) \n", _action_x, _action_y, _action_z, _action_yaw);
  */

  // Ensure that the drone's position is in accepted range error
  if ((fabs(_error_x) <= _tolerance) && (fabs(_error_y) <= _tolerance) && (fabs(_error_z) <= _tolerance))
  {
    // Since x,y,z are OK, now we need to tune yaw. Only when its done, procceed
    if (controlYaw(current_goal_yaw, pose_yaw, _dt))
    {
      if (_must_exit == true && !_hovering)
      {
        ROS_INFO("[Navigate] Final waypoint reached. Hovering...\n");
        _hovering = true;
        _waypoints_received = false;  // Set it to false again, so to wait for new waypoints to serve
        std_msgs::Bool feedback;
        feedback.data = true;
        // Provide feedback for the next waypoint to reach
        _goal_reached_pub.publish(feedback);
      }
      else if (_must_exit == true && _hovering)
        return;
      else
      {
        ROS_INFO("[Navigate] Error in accepted range. Next waypoint.\n");
        _current_goal = _waypoints.front();
        _waypoints.pop();  // Remove the element from the queue
        _waypoint_number += 1;
        _rise += 1;

        ROS_INFO("[Navigate] Next goal %d\n", _waypoint_number);
        ROS_INFO("[Navigate] Coordinates (x,y,z, yaw) : (%f, %f, %f, %f)\n", _current_goal.translation.x,
                 _current_goal.translation.y, _current_goal.translation.z, current_goal_yaw);
        // Do not publish twist that are related to the waypoint, since we can now proceed to waypoint + 1
        return;
      }
    }
  }

  if (_waypoint_number == _number_of_waypoints)
  {
    _must_exit = true;
    // Make the tolerance for the last waypoint, more strict
    float final_tolerance;
    _nh.param<float>("/tolerance", final_tolerance, 0.15);
    _tolerance = final_tolerance / 2;  // Make the tolerance for the last waypoint, more strict
  }

  _twist_stamped.twist = _twist;
  _twist_stamped.header.stamp = ros::Time::now();

  _vel_pub.publish(_twist);
  _stamped_vel_pub.publish(_twist_stamped);
}

bool Navigator::controlYaw(double current_goal_yaw, double pose_yaw, double dt)
{
  _error_yaw = current_goal_yaw - pose_yaw;
  if (fabs(_error_yaw) <= _yaw_tolerance)
  {
    return true;
  }
  else
  {
    _proportional_yaw = _yaw_kp * _error_yaw;
    _integral_yaw = _yaw_ki * (_integral_yaw + _error_yaw * dt);
    _derivative_yaw = _yaw_kd * (_error_yaw - _prev_error_yaw) / dt;
    _prev_error_yaw = _error_yaw;
    _action_yaw = _proportional_yaw + _integral_yaw + _derivative_yaw;
    clamp(_action_yaw, _rot_max_speed);
    _twist.linear.x = 0;
    _twist.linear.y = 0;
    _twist.linear.z = 0;
    _twist.angular.x = 0;
    _twist.angular.y = 0;
    _twist.angular.z = _action_yaw;
  }
  return false;
}

void Navigator::clamp(float& action, float max_action)
{
  if (fabs(action) > max_action)
  {
    // The first () gives us the sign of the action. If it is greater than max, then give it the max value
    action = ((action > 0) - (action < 0)) * max_action;
  }
  return;
}

}  // namespace navigate

int main(int argc, char** argv)
{
  ros::init(argc, argv, "navigate");

  navigate::Navigator nav;

  ros::spin();
  return 0;
}
