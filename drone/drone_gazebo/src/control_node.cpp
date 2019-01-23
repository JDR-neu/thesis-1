/* This ROS node implements the control of hector quadrotor to a specific height
 */

#include "drone_gazebo/control.h"

/* Main function */
int main(int argc, char** argv)
{
  ros::init(argc, argv, "control");

  if (argc != 2)
  {
    ROS_INFO("\n[USAGE] rosrun drone_gazebo control <desired_height_value>\n");
    return 1;
  }
  else
  {
    control::Controller controller(argv);
    ros::spin();
  }

  return 0;
}
