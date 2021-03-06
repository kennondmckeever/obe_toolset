#include <ros/ros.h> //For ROS itself
#include <opencv2/highgui/highgui.hpp> //FIXME Not exactly sure why this one is here; maybe it's for displaying the images that come in?
#include <cv_bridge/cv_bridge.h> //The openCV bridge that image_transport needs
#include <ros/console.h> //This is for ROS_ERROR and stuff.
#include <boost/filesystem.hpp> //For creating directtories if needed
#include <obe_toolset/ImageAndPose.h> //Needed for the custom Image and Pose message type
#include <string>
#include <vector>
//#include <GeographicLib/UTMUPS.hpp> //for conversion from Lat Lon to UTM. In order to get this, download the source, unzip it, cd into it, mkdir build, cd build, cmake .., make, make install
#include <sensor_msgs/NavSatFix.h> //for the mavros NavSatFix message
#include <sensor_msgs/Imu.h> // for the Imu data message
#include <tf/transform_datatypes.h> //for quaternion to yaw

//#include "std_msgs/String.h"
//#define ONEATATIME //if this is defined, it will do one file at a time and pause in between files.

//Global variables and classes that need to be accessed in the callback =(
double curr_lat, prev_lat;
double curr_lon, prev_lon;
double curr_z, prev_z;
ros::Time curr_timestamp, prev_timestamp;
static const double SEARCH_AREA_ALTITUDE = 4.2672; //This needs to be updated for camera agl altitude location calculations to be correct in the Processor nodes. For the competition in MD, it's about 14 feet (from google maps, guestimate)
int fix_wait_ctr = -2;

double curr_yaw;
bool yaw_valid = false;

void orientationCallback(const sensor_msgs::Imu msg)
{
	yaw_valid = true;
	tf::Quaternion quat;
	tf::quaternionMsgToTF(msg.orientation, quat);
	double dummy1, dummy2;
	tf::Matrix3x3(quat).getRPY(dummy1, dummy2, curr_yaw); //this is how we update yaw
}

void locationCallback(const sensor_msgs::NavSatFix msg) //all this does is update the global variables listed above (for location).
{
	if (0) //(msg.status == -1)
	{
		return; //we don't have a fix and can't update anything =/
	}
	else
	{
		if (fix_wait_ctr < 0)
			fix_wait_ctr += 1;
		//move the current data to the old data slot
		prev_lat = curr_lat;
		prev_lon = curr_lon;
		prev_z = curr_z;
		prev_timestamp = curr_timestamp;

		curr_lat = msg.latitude;
		curr_lon = msg.longitude;
		curr_z = msg.altitude - SEARCH_AREA_ALTITUDE; //this is 14 for the base in Maryland (makes since that it's pretty low; it's a naval base that sits on the ocean.
		curr_timestamp = msg.header.stamp;
	}
}

int main(int argc, char** argv)
{
	ros::init(argc, argv, "image_publisher_node");
	ros::NodeHandle n;
	int numOfPipes;
	int currentPipeMinusOne = 0;
	if(!(n.hasParam("numPipes")))
		ROS_INFO("the parameter 'numPipes' for the dispatcher node hasn't been specified; assuming 1 to ensure that no images are lost. This may cause severe back-up issues in a long mission.");
	n.param<int>("numPipes", numOfPipes, 1); //gets the numPipes param (so that we know where to publish), defaulting to 1 if it can't be read.

	n.subscribe("/mavros/global_position/global", 0, locationCallback);
	std::vector<ros::Publisher> impose_pub_vector(numOfPipes); //vector of publishers
	for(int i = 1; i <= numOfPipes; ++i)
	{
		std::string topic("obe/imagePipe");
		topic += std::to_string(i);
		impose_pub_vector[i-1] = n.advertise<obe_toolset::ImageAndPose>(topic.c_str(), 512); //publishes to the obe/imagePipe<i> and buffers up to 512 images per pipe in case it can't send them all that quickly.
	}

	//This block makes sure that the needed directories are set up (I'm pretty sure they should be, since this node might end up running from one of them).
	namespace fs = boost::filesystem;

		//NOTE: Not sure how relative paths will be handled when a launchfile runs this remotely...
	fs::path new_path("../images/new/"); //Will be used to get images.
	fs::path processed_path("../images/processed/");
	fs::path roi_path("../images/rois/");
	fs::path error_path("../images/unsuccessful/");

	fs::path im_fetch_path(new_path);

	fs::create_directories(im_fetch_path);
	fs::create_directories(processed_path);
	fs::create_directories(roi_path);
	fs::create_directories(error_path);
	//NOTE:: IF THERE IS AN ISSUE WITH PERMISSIONS FOR SOME REASON, IT MIGHT BE THAT THE LINES OF CODE ABOVE ARE REMOVING EXECUTE PERMISSIONS. JUST SOMETHING TO CONSIDER


	if (!(fs::exists(im_fetch_path)))
	{
		//If it doesn't exist, something went horribly wrong =/
		ROS_ERROR("Oh no. '%s' isn't a directory =(", im_fetch_path.string().c_str());
		return(-1);
	}
	else if (fs::is_regular_file(im_fetch_path))
	{
		ROS_ERROR("Oh no. '%s' is a regular file =(", im_fetch_path.string().c_str());
		return(-1);
	}

	#ifdef ONEATATIME
	ros::Rate loop_rate(.25);
	#else
	ros::Rate loop_rate(25); //rate is the number of outer loop iterations per second
	//Also, we need the loop rate to be relatively quick becuase we don't want delay in dispatching images. Delay in dispatching images could lead to wasted execution time and potential incorrect location stamps.
	#endif //ONEATATIME

	while(n.ok())
	{
		//We need to first make sure that the GPS Fix is ready (should take 1 second if the GPS is already "warmed up"
		if(fix_wait_ctr)
		{
			ROS_INFO_THROTTLE(5,"Waiting for GPS Fix"); //Display at most once every 5 seconds
		}
		else
		{
			//We need to dispatch each of the images in the new directory
			fs::directory_iterator end_itr;
			for(fs::directory_iterator cur_path_itr(im_fetch_path); cur_path_itr != end_itr; cur_path_itr++)
			{
				ROS_INFO("I'm looking at this file: %s", cur_path_itr->path().string().c_str());
				if(fs::is_directory(cur_path_itr->path()))
				{
					ROS_INFO_STREAM_ONCE("There's a directory in the ~/images/new path; it is being moved to the error folder.");
					fs::rename(cur_path_itr->path(), error_path / cur_path_itr->path().filename());
					//FIXME Don't forget to move this file to the folder for files with errors.

				}
				else
				{
					cv::Mat newImage = cv::imread(cur_path_itr->path().string(), CV_LOAD_IMAGE_COLOR);

					if(newImage.data == NULL)//The image was read improperly if this fails...
					{
						ROS_ERROR("I was unable to read the image file.");
						fs::rename(cur_path_itr->path(), error_path / cur_path_itr->path().filename());
					}
					else
					{
						obe_toolset::ImageAndPose impose_msg; //This is the custom message type that has a sensor_msgs::Image msg and a std_msgs::Pose message.
						impose_msg.image = *(cv_bridge::CvImage(std_msgs::Header(), "bgr8", newImage).toImageMsg()); //The meat of this line is from the image_transport tutorial; I just de-reference their piece to get a sensor_msgs::Image.  Note: There's probably a better way to do this, but it will work for now.

						//This is the point where the fakeing things comes in.
						ros::Duration elapsedTime = curr_timestamp - prev_timestamp;
						double lat_offset = (2 / elapsedTime.toSec()) * (curr_lat - prev_lat); //the "2" is from Rick's estimate of time between when an image is captured and when it is finally done uploading.
						impose_msg.lat = curr_lat - lat_offset; //approximates a latitude value (linear approximation based on 2 most recent locations)
						double lon_offset = (2 / elapsedTime.toSec()) * (curr_lon - prev_lon);
						impose_msg.lon = curr_lon - lon_offset;
						double z_offset = (2 / elapsedTime.toSec()) * (curr_z - prev_z);
						impose_msg.z = curr_z - z_offset; //This is actually a decent guess (~200 ft)
						impose_msg.roll = 0.0;
						impose_msg.pitch = 0.0;
						//Concerning Yaw: we really should be using the /mavros/imu/data to give us the yaw, particularly because the plane could be "crabbing" (flying in a direction different than the one it's pointing due to wind)
						//But for now, it's simply found by 
						impose_msg.yaw = 92929292; //this is the only one that's used as of 4.26.17
						//End the faking it stuff.

						//publish to the current pipe that's due for another message. NOTE: In the future, this could have a system that keeps track of busy nodes so that no particular node gets bogged down. I'm kind of assuming that we have enough nodes and a fast enough ROI algorithm and randomness is on our side so that this doesn't get out of hand.
						impose_pub_vector[currentPipeMinusOne].publish(impose_msg); //send the impose message.

						//set up the current pipe for the next time we publish something.
						currentPipeMinusOne++;
						currentPipeMinusOne %= numOfPipes; //we want to wrap around

						//Now that we've published it, we can move the file to the processed folder
						fs::rename(cur_path_itr->path(), processed_path / cur_path_itr->path().filename());
						#ifdef ONEATATIME
						break;
						#endif
					}
				}
			} //Ends the for loop
		}//ends the if statement that waits for a GPS Fix
		ros::spinOnce();
		loop_rate.sleep();
	}
	//It shouldn't get here until you hit ctrl-C, but we still need to specify a return value:
	return 0;
}
