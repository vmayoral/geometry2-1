#include "old_buffer_core.h"
#include "../include/tf2/time_cache.h"
#include "../include/tf2/exceptions.h"
#include "tf2_msgs/TF2Error.h"

#include <cassert>
#include <console_bridge/console.h>
#include "../include/tf2/LinearMath/Transform.h"
#include <boost/foreach.hpp>

namespace old_tf2
{

// Tolerance for acceptable quaternion normalization
  static double QUATERNION_NORMALIZATION_TOLERANCE = 10e-3;

/** \brief convert Transform msg to Transform */
  void transformMsgToTF2(const geometry_msgs::Transform& msg, tf2::Transform& tf2)
  {tf2 = tf2::Transform(tf2::Quaternion(msg.rotation.x, msg.rotation.y, msg.rotation.z, msg.rotation.w), tf2::Vector3(msg.translation.x, msg.translation.y, msg.translation.z));}

/** \brief convert Transform to Transform msg*/
  void transformTF2ToMsg(const tf2::Transform& tf2, geometry_msgs::Transform& msg)
  {
    msg.translation.x = tf2.getOrigin().x();
    msg.translation.y = tf2.getOrigin().y();
    msg.translation.z = tf2.getOrigin().z();
    msg.rotation.x = tf2.getRotation().x();
    msg.rotation.y = tf2.getRotation().y();
    msg.rotation.z = tf2.getRotation().z();
    msg.rotation.w = tf2.getRotation().w();
  }

/** \brief convert Transform to Transform msg*/
  void transformTF2ToMsg(const tf2::Transform& tf2, geometry_msgs::TransformStamped& msg, ros::Time stamp, const std::string& frame_id, const std::string& child_frame_id)
  {
    transformTF2ToMsg(tf2, msg.transform);
    msg.header.stamp = stamp;
    msg.header.frame_id = frame_id;
    msg.child_frame_id = child_frame_id;
  }

  void transformTF2ToMsg(const tf2::Quaternion& orient, const tf2::Vector3& pos, geometry_msgs::Transform& msg)
  {
    msg.translation.x = pos.x();
    msg.translation.y = pos.y();
    msg.translation.z = pos.z();
    msg.rotation.x = orient.x();
    msg.rotation.y = orient.y();
    msg.rotation.z = orient.z();
    msg.rotation.w = orient.w();
  }

  void transformTF2ToMsg(const tf2::Quaternion& orient, const tf2::Vector3& pos, geometry_msgs::TransformStamped& msg, ros::Time stamp, const std::string& frame_id, const std::string& child_frame_id)
  {
    transformTF2ToMsg(orient, pos, msg.transform);
    msg.header.stamp = stamp;
    msg.header.frame_id = frame_id;
    msg.child_frame_id = child_frame_id;
  }

  void setIdentity(geometry_msgs::Transform& tx)
  {
    tx.translation.x = 0;
    tx.translation.y = 0;
    tx.translation.z = 0;
    tx.rotation.x = 0;
    tx.rotation.y = 0;
    tx.rotation.z = 0;
    tx.rotation.w = 1;
  }

  bool startsWithSlash(const std::string& frame_id)
  {
    if (frame_id.size() > 0)
      if (frame_id[0] == '/')
        return true;
    return false;
  }

  std::string stripSlash(const std::string& in)
  {
    std::string out = in;
    if (startsWithSlash(in))
      out.erase(0,1);
    return out;
  }


  bool OldBufferCore::warnFrameId(const char* function_name_arg, const std::string& frame_id) const
  {
    if (frame_id.size() == 0)
    {
      std::stringstream ss;
      ss << "Invalid argument passed to "<< function_name_arg <<" in tf2 frame_ids cannot be empty";
      CONSOLE_BRIDGE_logWarn("%s",ss.str().c_str());
      return true;
    }

    if (startsWithSlash(frame_id))
    {
      std::stringstream ss;
      ss << "Invalid argument \"" << frame_id << "\" passed to "<< function_name_arg <<" in tf2 frame_ids cannot start with a '/' like: ";
      CONSOLE_BRIDGE_logWarn("%s",ss.str().c_str());
      return true;
    }

    return false;
  }

  tf2::CompactFrameID OldBufferCore::validateFrameId(const char* function_name_arg, const std::string& frame_id) const
  {
    if (frame_id.empty())
    {
      std::stringstream ss;
      ss << "Invalid argument passed to "<< function_name_arg <<" in tf2 frame_ids cannot be empty";
      throw tf2::InvalidArgumentException(ss.str().c_str());
    }

    if (startsWithSlash(frame_id))
    {
      std::stringstream ss;
      ss << "Invalid argument \"" << frame_id << "\" passed to "<< function_name_arg <<" in tf2 frame_ids cannot start with a '/' like: ";
      throw tf2::InvalidArgumentException(ss.str().c_str());
    }

    tf2::CompactFrameID id = lookupFrameNumber(frame_id);
    if (id == 0)
    {
      std::stringstream ss;
      ss << "\"" << frame_id << "\" passed to "<< function_name_arg <<" does not exist. ";
      throw tf2::LookupException(ss.str().c_str());
    }

    return id;
  }

  OldBufferCore::OldBufferCore(ros::Duration cache_time)
    : cache_time_(cache_time)
    , transformable_callbacks_counter_(0)
    , transformable_requests_counter_(0)
    , using_dedicated_thread_(false)
  {
    frameIDs_["NO_PARENT"] = 0;
    frames_.push_back(TimeCacheInterfacePtr());
    frameIDs_reverse.push_back("NO_PARENT");
  }

  OldBufferCore::~OldBufferCore()
  {

  }

  void OldBufferCore::clear()
  {
    //old_tf_.clear();


    boost::mutex::scoped_lock lock(frame_mutex_);
    if ( frames_.size() > 1 )
    {
      for (std::vector<TimeCacheInterfacePtr>::iterator  cache_it = frames_.begin() + 1; cache_it != frames_.end(); ++cache_it)
      {
        if (*cache_it)
          (*cache_it)->clearList();
      }
    }

  }

  bool OldBufferCore::setTransform(const geometry_msgs::TransformStamped& transform_in, const std::string& authority, bool is_static)
  {

    /////BACKEARDS COMPATABILITY
    /* tf::StampedTransform tf_transform;
    tf::transformStampedMsgToTF(transform_in, tf_transform);
    if  (!old_tf_.setTransform(tf_transform, authority))
    {
      printf("Warning old setTransform Failed but was not caught\n");
      }*/

    /////// New implementation
    geometry_msgs::TransformStamped stripped = transform_in;
    stripped.header.frame_id = stripSlash(stripped.header.frame_id);
    stripped.child_frame_id = stripSlash(stripped.child_frame_id);


    bool error_exists = false;
    if (stripped.child_frame_id == stripped.header.frame_id)
    {
      CONSOLE_BRIDGE_logError("TF_SELF_TRANSFORM: Ignoring transform from authority \"%s\" with frame_id and child_frame_id  \"%s\" because they are the same",  authority.c_str(), stripped.child_frame_id.c_str());
      error_exists = true;
    }

    if (stripped.child_frame_id == "")
    {
      CONSOLE_BRIDGE_logError("TF_NO_CHILD_FRAME_ID: Ignoring transform from authority \"%s\" because child_frame_id not set ", authority.c_str());
      error_exists = true;
    }

    if (stripped.header.frame_id == "")
    {
      CONSOLE_BRIDGE_logError("TF_NO_FRAME_ID: Ignoring transform with child_frame_id \"%s\"  from authority \"%s\" because frame_id not set", stripped.child_frame_id.c_str(), authority.c_str());
      error_exists = true;
    }

    if (std::isnan(stripped.transform.translation.x) || std::isnan(stripped.transform.translation.y) || std::isnan(stripped.transform.translation.z)||
        std::isnan(stripped.transform.rotation.x) ||       std::isnan(stripped.transform.rotation.y) ||       std::isnan(stripped.transform.rotation.z) ||       std::isnan(stripped.transform.rotation.w))
    {
      CONSOLE_BRIDGE_logError("TF_NAN_INPUT: Ignoring transform for child_frame_id \"%s\" from authority \"%s\" because of a nan value in the transform (%f %f %f) (%f %f %f %f)",
                              stripped.child_frame_id.c_str(), authority.c_str(),
                              stripped.transform.translation.x, stripped.transform.translation.y, stripped.transform.translation.z,
                              stripped.transform.rotation.x, stripped.transform.rotation.y, stripped.transform.rotation.z, stripped.transform.rotation.w
      );
      error_exists = true;
    }

    bool valid = std::abs((stripped.transform.rotation.w * stripped.transform.rotation.w
                           + stripped.transform.rotation.x * stripped.transform.rotation.x
                           + stripped.transform.rotation.y * stripped.transform.rotation.y
                           + stripped.transform.rotation.z * stripped.transform.rotation.z) - 1.0f) < QUATERNION_NORMALIZATION_TOLERANCE;

    if (!valid)
    {
      CONSOLE_BRIDGE_logError("TF_DENORMALIZED_QUATERNION: Ignoring transform for child_frame_id \"%s\" from authority \"%s\" because of an invalid quaternion in the transform (%f %f %f %f)",
                              stripped.child_frame_id.c_str(), authority.c_str(),
                              stripped.transform.rotation.x, stripped.transform.rotation.y, stripped.transform.rotation.z, stripped.transform.rotation.w);
      error_exists = true;
    }

    if (error_exists)
      return false;

    {
      boost::mutex::scoped_lock lock(frame_mutex_);
      tf2::CompactFrameID frame_number = lookupOrInsertFrameNumber(stripped.child_frame_id);
      TimeCacheInterfacePtr frame = getFrame(frame_number);
      if (frame == NULL)
        frame = allocateFrame(frame_number, is_static);

      if (frame->insertData(tf2::TransformStorage(stripped, lookupOrInsertFrameNumber(stripped.header.frame_id), frame_number)))
      {
        frame_authority_[frame_number] = authority;
      }
      else
      {
        CONSOLE_BRIDGE_logWarn("TF_OLD_DATA ignoring data from the past for frame %s at time %g according to authority %s\nPossible reasons are listed at http://wiki.ros.org/tf/Errors%%20explained", stripped.child_frame_id.c_str(), stripped.header.stamp.toSec(), authority.c_str());
        return false;
      }
    }

    testTransformableRequests();

    return true;
  }

  TimeCacheInterfacePtr OldBufferCore::allocateFrame(tf2::CompactFrameID cfid, bool is_static)
  {
    TimeCacheInterfacePtr frame_ptr = frames_[cfid];
    if (is_static) {
      frames_[cfid] = TimeCacheInterfacePtr(new tf2::StaticCache());
    } else {
      frames_[cfid] = TimeCacheInterfacePtr(new tf2::TimeCache(cache_time_));
    }

    return frames_[cfid];
  }

  enum WalkEnding
  {
    Identity,
    TargetParentOfSource,
    SourceParentOfTarget,
    FullPath,
  };

// TODO for Jade: Merge walkToTopParent functions; this is now a stub to preserve ABI
  template<typename F>
  int OldBufferCore::walkToTopParent(F& f, ros::Time time, tf2::CompactFrameID target_id, tf2::CompactFrameID source_id, std::string* error_string) const
  {
    return walkToTopParent(f, time, target_id, source_id, error_string, NULL);
  }

  template<typename F>
  int OldBufferCore::walkToTopParent(F& f, ros::Time time, tf2::CompactFrameID target_id,
                                  tf2::CompactFrameID source_id, std::string* error_string, std::vector<tf2::CompactFrameID>
                                  *frame_chain) const
  {
    if (frame_chain)
      frame_chain->clear();

    // Short circuit if zero length transform to allow lookups on non existant links
    if (source_id == target_id)
    {
      f.finalize(Identity, time);
      return tf2_msgs::TF2Error::NO_ERROR;
    }

    //If getting the latest get the latest common time
    if (time == ros::Time())
    {
      int retval = getLatestCommonTime(target_id, source_id, time, error_string);
      if (retval != tf2_msgs::TF2Error::NO_ERROR)
      {
        return retval;
      }
    }

    // Walk the tree to its root from the source frame, accumulating the transform
    tf2::CompactFrameID frame = source_id;
    tf2::CompactFrameID top_parent = frame;
    uint32_t depth = 0;

    std::string extrapolation_error_string;
    bool extrapolation_might_have_occurred = false;

    while (frame != 0)
    {
      TimeCacheInterfacePtr cache = getFrame(frame);
      if (frame_chain)
        frame_chain->push_back(frame);

      if (!cache)
      {
        // There will be no cache for the very root of the tree
        top_parent = frame;
        break;
      }

      tf2::CompactFrameID parent = f.gather(cache, time, &extrapolation_error_string);
      if (parent == 0)
      {
        // Just break out here... there may still be a path from source -> target
        top_parent = frame;
        extrapolation_might_have_occurred = true;
        break;
      }

      // Early out... target frame is a direct parent of the source frame
      if (frame == target_id)
      {
        f.finalize(TargetParentOfSource, time);
        return tf2_msgs::TF2Error::NO_ERROR;
      }

      f.accum(true);

      top_parent = frame;
      frame = parent;

      ++depth;
      if (depth > MAX_GRAPH_DEPTH)
      {
        if (error_string)
        {
          std::stringstream ss;
          ss << "The tf tree is invalid because it contains a loop." << std::endl
             << allFramesAsStringNoLock() << std::endl;
          *error_string = ss.str();
        }
        return tf2_msgs::TF2Error::LOOKUP_ERROR;
      }
    }

    // Now walk to the top parent from the target frame, accumulating its transform
    frame = target_id;
    depth = 0;
    std::vector<tf2::CompactFrameID> reverse_frame_chain;

    while (frame != top_parent)
    {
      TimeCacheInterfacePtr cache = getFrame(frame);
      if (frame_chain)
        reverse_frame_chain.push_back(frame);

      if (!cache)
      {
        break;
      }

      tf2::CompactFrameID parent = f.gather(cache, time, error_string);
      if (parent == 0)
      {
        if (error_string)
        {
          std::stringstream ss;
          ss << *error_string << ", when looking up transform from frame [" << lookupFrameString(source_id) << "] to frame [" << lookupFrameString(target_id) << "]";
          *error_string = ss.str();
        }

        return tf2_msgs::TF2Error::EXTRAPOLATION_ERROR;
      }

      // Early out... source frame is a direct parent of the target frame
      if (frame == source_id)
      {
        f.finalize(SourceParentOfTarget, time);
        if (frame_chain)
        {
          // Use the walk we just did
          frame_chain->swap(reverse_frame_chain);
          // Reverse it before returning because this is the reverse walk.
          std::reverse(frame_chain->begin(), frame_chain->end());
        }
        return tf2_msgs::TF2Error::NO_ERROR;
      }

      f.accum(false);

      frame = parent;

      ++depth;
      if (depth > MAX_GRAPH_DEPTH)
      {
        if (error_string)
        {
          std::stringstream ss;
          ss << "The tf tree is invalid because it contains a loop." << std::endl
             << allFramesAsStringNoLock() << std::endl;
          *error_string = ss.str();
        }
        return tf2_msgs::TF2Error::LOOKUP_ERROR;
      }
    }

    if (frame != top_parent)
    {
      if (extrapolation_might_have_occurred)
      {
        if (error_string)
        {
          std::stringstream ss;
          ss << extrapolation_error_string << ", when looking up transform from frame [" << lookupFrameString(source_id) << "] to frame [" << lookupFrameString(target_id) << "]";
          *error_string = ss.str();
        }

        return tf2_msgs::TF2Error::EXTRAPOLATION_ERROR;

      }

      createConnectivityErrorString(source_id, target_id, error_string);
      return tf2_msgs::TF2Error::CONNECTIVITY_ERROR;
    }
    else if (frame_chain){
      // append top_parent to reverse_frame_chain for easier matching/trimming
      reverse_frame_chain.push_back(frame);
    }

    f.finalize(FullPath, time);
    if (frame_chain)
    {
      // Pruning: Compare the chains starting at the parent (end) until they differ
      int m = reverse_frame_chain.size()-1;
      int n = frame_chain->size()-1;
      for (; m >= 0 && n >= 0; --m, --n)
      {
        if ((*frame_chain)[n] != reverse_frame_chain[m])
        {
          break;
        }
      }
      // Erase all duplicate items from frame_chain
      if (n > 0)
      {
        // N is offset by 1 and leave the common parent for this result
        frame_chain->erase(frame_chain->begin() + (n + 2), frame_chain->end());
      }
      if (m < reverse_frame_chain.size())
      {
        for (int i = m; i >= 0; --i)
        {
          frame_chain->push_back(reverse_frame_chain[i]);
        }
      }
    }

    return tf2_msgs::TF2Error::NO_ERROR;
  }



  struct TransformAccum
  {
    TransformAccum()
      : source_to_top_quat(0.0, 0.0, 0.0, 1.0)
      , source_to_top_vec(0.0, 0.0, 0.0)
      , target_to_top_quat(0.0, 0.0, 0.0, 1.0)
      , target_to_top_vec(0.0, 0.0, 0.0)
      , result_quat(0.0, 0.0, 0.0, 1.0)
      , result_vec(0.0, 0.0, 0.0)
    {
    }

    tf2::CompactFrameID gather(TimeCacheInterfacePtr cache, ros::Time time, std::string* error_string)
    {
      if (!cache->getData(time, st, error_string))
      {
        return 0;
      }

      return st.frame_id_;
    }

    void accum(bool source)
    {
      if (source)
      {
        source_to_top_vec = quatRotate(st.rotation_, source_to_top_vec) + st.translation_;
        source_to_top_quat = st.rotation_ * source_to_top_quat;
      }
      else
      {
        target_to_top_vec = quatRotate(st.rotation_, target_to_top_vec) + st.translation_;
        target_to_top_quat = st.rotation_ * target_to_top_quat;
      }
    }

    void finalize(WalkEnding end, ros::Time _time)
    {
      switch (end)
      {
        case Identity:
          break;
        case TargetParentOfSource:
          result_vec = source_to_top_vec;
          result_quat = source_to_top_quat;
          break;
        case SourceParentOfTarget:
        {
          tf2::Quaternion inv_target_quat = target_to_top_quat.inverse();
          tf2::Vector3 inv_target_vec = quatRotate(inv_target_quat, -target_to_top_vec);
          result_vec = inv_target_vec;
          result_quat = inv_target_quat;
          break;
        }
        case FullPath:
        {
          tf2::Quaternion inv_target_quat = target_to_top_quat.inverse();
          tf2::Vector3 inv_target_vec = quatRotate(inv_target_quat, -target_to_top_vec);

          result_vec = quatRotate(inv_target_quat, source_to_top_vec) + inv_target_vec;
          result_quat = inv_target_quat * source_to_top_quat;
        }
          break;
      };

      time = _time;
    }

    tf2::TransformStorage st;
    ros::Time time;
    tf2::Quaternion source_to_top_quat;
    tf2::Vector3 source_to_top_vec;
    tf2::Quaternion target_to_top_quat;
    tf2::Vector3 target_to_top_vec;

    tf2::Quaternion result_quat;
    tf2::Vector3 result_vec;
  };

  void OldBufferCore::justReadFrames(const std::vector<std::string> &frames) const{
    boost::mutex::scoped_lock lock(frame_mutex_);
    tf2::TransformStorage __attribute__((used)) st{};

    for(auto &frame_str: frames){
      auto frame_id = lookupFrameNumber(frame_str);

      if(frame_id != 0){
        auto frame = getFrame(frame_id);
        frame->getData(ros::Time(0), st, nullptr);
      }
    }

    // just access and do nothing
  }

  geometry_msgs::TransformStamped OldBufferCore::lookupTransform(const std::string& target_frame,
                                                              const std::string& source_frame,
                                                              const ros::Time& time) const
  {
    boost::mutex::scoped_lock lock(frame_mutex_);

    if (target_frame == source_frame) {
      geometry_msgs::TransformStamped identity;
      identity.header.frame_id = target_frame;
      identity.child_frame_id = source_frame;
      identity.transform.rotation.w = 1;

      if (time == ros::Time())
      {
        tf2::CompactFrameID target_id = lookupFrameNumber(target_frame);
        TimeCacheInterfacePtr cache = getFrame(target_id);
        if (cache)
          identity.header.stamp = cache->getLatestTimestamp();
        else
          identity.header.stamp = time;
      }
      else
        identity.header.stamp = time;

      return identity;
    }

    //Identify case does not need to be validated above
    tf2::CompactFrameID target_id = validateFrameId("lookupTransform argument target_frame", target_frame);
    tf2::CompactFrameID source_id = validateFrameId("lookupTransform argument source_frame", source_frame);

    std::string error_string;
    TransformAccum accum;
    int retval = walkToTopParent(accum, time, target_id, source_id, &error_string);
    if (retval != tf2_msgs::TF2Error::NO_ERROR)
    {
      switch (retval)
      {
        case tf2_msgs::TF2Error::CONNECTIVITY_ERROR:
          throw tf2::ConnectivityException(error_string);
        case tf2_msgs::TF2Error::EXTRAPOLATION_ERROR:
          throw tf2::ExtrapolationException(error_string);
        case tf2_msgs::TF2Error::LOOKUP_ERROR:
          throw tf2::LookupException(error_string);
        default:
          CONSOLE_BRIDGE_logError("Unknown error code: %d", retval);
          assert(0);
      }
    }

    geometry_msgs::TransformStamped output_transform;
    transformTF2ToMsg(accum.result_quat, accum.result_vec, output_transform, accum.time, target_frame, source_frame);
    return output_transform;
  }


  geometry_msgs::TransformStamped OldBufferCore::lookupTransform(const std::string& target_frame,
                                                              const ros::Time& target_time,
                                                              const std::string& source_frame,
                                                              const ros::Time& source_time,
                                                              const std::string& fixed_frame) const
  {
    validateFrameId("lookupTransform argument target_frame", target_frame);
    validateFrameId("lookupTransform argument source_frame", source_frame);
    validateFrameId("lookupTransform argument fixed_frame", fixed_frame);

    geometry_msgs::TransformStamped output;
    geometry_msgs::TransformStamped temp1 =  lookupTransform(fixed_frame, source_frame, source_time);
    geometry_msgs::TransformStamped temp2 =  lookupTransform(target_frame, fixed_frame, target_time);

    tf2::Transform tf1, tf2;
    transformMsgToTF2(temp1.transform, tf1);
    transformMsgToTF2(temp2.transform, tf2);
    transformTF2ToMsg(tf2*tf1, output.transform);
    output.header.stamp = temp2.header.stamp;
    output.header.frame_id = target_frame;
    output.child_frame_id = source_frame;
    return output;
  }



/*
geometry_msgs::Twist OldBufferCore::lookupTwist(const std::string& tracking_frame,
                                          const std::string& observation_frame,
                                          const ros::Time& time,
                                          const ros::Duration& averaging_interval) const
{
  try
  {
  geometry_msgs::Twist t;
  old_tf_.lookupTwist(tracking_frame, observation_frame,
                      time, averaging_interval, t);
  return t;
  }
  catch (tf::LookupException& ex)
  {
    throw tf2::LookupException(ex.what());
  }
  catch (tf::ConnectivityException& ex)
  {
    throw tf2::ConnectivityException(ex.what());
  }
  catch (tf::ExtrapolationException& ex)
  {
    throw tf2::ExtrapolationException(ex.what());
  }
  catch (tf::InvalidArgument& ex)
  {
    throw tf2::InvalidArgumentException(ex.what());
  }
}

geometry_msgs::Twist OldBufferCore::lookupTwist(const std::string& tracking_frame,
                                          const std::string& observation_frame,
                                          const std::string& reference_frame,
                                          const tf2::Point & reference_point,
                                          const std::string& reference_point_frame,
                                          const ros::Time& time,
                                          const ros::Duration& averaging_interval) const
{
  try{
  geometry_msgs::Twist t;
  old_tf_.lookupTwist(tracking_frame, observation_frame, reference_frame, reference_point, reference_point_frame,
                      time, averaging_interval, t);
  return t;
  }
  catch (tf::LookupException& ex)
  {
    throw tf2::LookupException(ex.what());
  }
  catch (tf::ConnectivityException& ex)
  {
    throw tf2::ConnectivityException(ex.what());
  }
  catch (tf::ExtrapolationException& ex)
  {
    throw tf2::ExtrapolationException(ex.what());
  }
  catch (tf::InvalidArgument& ex)
  {
    throw tf2::InvalidArgumentException(ex.what());
  }
}
*/

  struct CanTransformAccum
  {
    tf2::CompactFrameID gather(TimeCacheInterfacePtr cache, ros::Time time, std::string* error_string)
    {
      return cache->getParent(time, error_string);
    }

    void accum(bool source)
    {
    }

    void finalize(WalkEnding end, ros::Time _time)
    {
    }

    tf2::TransformStorage st;
  };

  bool OldBufferCore::canTransformNoLock(tf2::CompactFrameID target_id, tf2::CompactFrameID source_id,
                                      const ros::Time& time, std::string* error_msg) const
  {
    if (target_id == 0 || source_id == 0)
    {
      if (error_msg)
      {
        if (target_id == 0)
        {
          *error_msg += std::string("target_frame: " + lookupFrameString(target_id ) + " does not exist.");
        }
        if (source_id == 0)
        {
          if (target_id == 0)
          {
            *error_msg += std::string(" ");
          }
          *error_msg += std::string("source_frame: " + lookupFrameString(source_id) + " " + lookupFrameString(source_id ) + " does not exist.");
        }
      }
      return false;
    }

    if (target_id == source_id)
    {
      return true;
    }

    CanTransformAccum accum;
    if (walkToTopParent(accum, time, target_id, source_id, error_msg) == tf2_msgs::TF2Error::NO_ERROR)
    {
      return true;
    }

    return false;
  }

  bool OldBufferCore::canTransformInternal(tf2::CompactFrameID target_id, tf2::CompactFrameID source_id,
                                        const ros::Time& time, std::string* error_msg) const
  {
    boost::mutex::scoped_lock lock(frame_mutex_);
    return canTransformNoLock(target_id, source_id, time, error_msg);
  }

  bool OldBufferCore::canTransform(const std::string& target_frame, const std::string& source_frame,
                                const ros::Time& time, std::string* error_msg) const
  {
    // Short circuit if target_frame == source_frame
    if (target_frame == source_frame)
      return true;

    if (warnFrameId("canTransform argument target_frame", target_frame))
      return false;
    if (warnFrameId("canTransform argument source_frame", source_frame))
      return false;

    boost::mutex::scoped_lock lock(frame_mutex_);

    tf2::CompactFrameID target_id = lookupFrameNumber(target_frame);
    tf2::CompactFrameID source_id = lookupFrameNumber(source_frame);

    if (target_id == 0 || source_id == 0)
    {
      if (error_msg)
      {
        if (target_id == 0)
        {
          *error_msg += std::string("canTransform: target_frame " + target_frame + " does not exist.");
        }
        if (source_id == 0)
        {
          if (target_id == 0)
          {
            *error_msg += std::string(" ");
          }
          *error_msg += std::string("canTransform: source_frame " + source_frame + " does not exist.");
        }
      }
      return false;
    }
    return canTransformNoLock(target_id, source_id, time, error_msg);
  }

  bool OldBufferCore::canTransform(const std::string& target_frame, const ros::Time& target_time,
                                const std::string& source_frame, const ros::Time& source_time,
                                const std::string& fixed_frame, std::string* error_msg) const
  {
    if (warnFrameId("canTransform argument target_frame", target_frame))
      return false;
    if (warnFrameId("canTransform argument source_frame", source_frame))
      return false;
    if (warnFrameId("canTransform argument fixed_frame", fixed_frame))
      return false;

    boost::mutex::scoped_lock lock(frame_mutex_);
    tf2::CompactFrameID target_id = lookupFrameNumber(target_frame);
    tf2::CompactFrameID source_id = lookupFrameNumber(source_frame);
    tf2::CompactFrameID fixed_id = lookupFrameNumber(fixed_frame);

    if (target_id == 0 || source_id == 0 || fixed_id == 0)
    {
      if (error_msg)
      {
        if (target_id == 0)
        {
          *error_msg += std::string("canTransform: target_frame " + target_frame + " does not exist.");
        }
        if (source_id == 0)
        {
          if (target_id == 0)
          {
            *error_msg += std::string(" ");
          }
          *error_msg += std::string("canTransform: source_frame " + source_frame + " does not exist.");
        }
        if (source_id == 0)
        {
          if (target_id == 0 || source_id == 0)
          {
            *error_msg += std::string(" ");
          }
          *error_msg += std::string("fixed_frame: " + fixed_frame + "does not exist.");
        }
      }
      return false;
    }
    return canTransformNoLock(target_id, fixed_id, target_time, error_msg) && canTransformNoLock(fixed_id, source_id, source_time, error_msg);
  }


  TimeCacheInterfacePtr OldBufferCore::getFrame(tf2::CompactFrameID frame_id) const
  {
    if (frame_id >= frames_.size())
      return TimeCacheInterfacePtr();
    else
    {
      return frames_[frame_id];
    }
  }

  tf2::CompactFrameID OldBufferCore::lookupFrameNumber(const std::string& frameid_str) const
  {
    tf2::CompactFrameID retval;
    M_StringToCompactFrameID::const_iterator map_it = frameIDs_.find(frameid_str);
    if (map_it == frameIDs_.end())
    {
      retval = tf2::CompactFrameID(0);
    }
    else
      retval = map_it->second;
    return retval;
  }

  tf2::CompactFrameID OldBufferCore::lookupOrInsertFrameNumber(const std::string& frameid_str)
  {
    tf2::CompactFrameID retval = 0;
    M_StringToCompactFrameID::iterator map_it = frameIDs_.find(frameid_str);
    if (map_it == frameIDs_.end())
    {
      retval = tf2::CompactFrameID(frames_.size());
      frames_.push_back(TimeCacheInterfacePtr());//Just a place holder for iteration
      frameIDs_[frameid_str] = retval;
      frameIDs_reverse.push_back(frameid_str);
    }
    else
      retval = frameIDs_[frameid_str];

    return retval;
  }

  const std::string& OldBufferCore::lookupFrameString(tf2::CompactFrameID frame_id_num) const
  {
    if (frame_id_num >= frameIDs_reverse.size())
    {
      std::stringstream ss;
      ss << "Reverse lookup of frame id " << frame_id_num << " failed!";
      throw tf2::LookupException(ss.str());
    }
    else
      return frameIDs_reverse[frame_id_num];
  }

  void OldBufferCore::createConnectivityErrorString(tf2::CompactFrameID source_frame, tf2::CompactFrameID target_frame, std::string* out) const
  {
    if (!out)
    {
      return;
    }
    *out = std::string("Could not find a connection between '"+lookupFrameString(target_frame)+"' and '"+
                       lookupFrameString(source_frame)+"' because they are not part of the same tree."+
                       "Tf has two or more unconnected trees.");
  }

  std::string OldBufferCore::allFramesAsString() const
  {
    boost::mutex::scoped_lock lock(frame_mutex_);
    return this->allFramesAsStringNoLock();
  }

  std::string OldBufferCore::allFramesAsStringNoLock() const
  {
    std::stringstream mstream;

    tf2::TransformStorage temp;

    //  for (std::vector< TimeCache*>::iterator  it = frames_.begin(); it != frames_.end(); ++it)

    ///regular transforms
    for (unsigned int counter = 1; counter < frames_.size(); counter ++)
    {
      TimeCacheInterfacePtr frame_ptr = getFrame(tf2::CompactFrameID(counter));
      if (frame_ptr == NULL)
        continue;
      tf2::CompactFrameID frame_id_num;
      if(  frame_ptr->getData(ros::Time(), temp))
        frame_id_num = temp.frame_id_;
      else
      {
        frame_id_num = 0;
      }
      mstream << "Frame "<< frameIDs_reverse[counter] << " exists with parent " << frameIDs_reverse[frame_id_num] << "." <<std::endl;
    }

    return mstream.str();
  }

  struct TimeAndFrameIDFrameComparator
  {
    TimeAndFrameIDFrameComparator(tf2::CompactFrameID id)
      : id(id)
    {}

    bool operator()(const P_TimeAndFrameID& rhs) const
    {
      return rhs.second == id;
    }

    tf2::CompactFrameID id;
  };

  int OldBufferCore::getLatestCommonTime(tf2::CompactFrameID target_id, tf2::CompactFrameID source_id, ros::Time & time, std::string * error_string) const
  {
    // Error if one of the frames don't exist.
    if (source_id == 0 || target_id == 0) return tf2_msgs::TF2Error::LOOKUP_ERROR;

    if (source_id == target_id)
    {
      TimeCacheInterfacePtr cache = getFrame(source_id);
      //Set time to latest timestamp of frameid in case of target and source frame id are the same
      if (cache)
        time = cache->getLatestTimestamp();
      else
        time = ros::Time();
      return tf2_msgs::TF2Error::NO_ERROR;
    }

    std::vector<P_TimeAndFrameID> lct_cache;

    // Walk the tree to its root from the source frame, accumulating the list of parent/time as well as the latest time
    // in the target is a direct parent
    tf2::CompactFrameID frame = source_id;
    P_TimeAndFrameID temp;
    uint32_t depth = 0;
    ros::Time common_time = ros::TIME_MAX;
    while (frame != 0)
    {
      TimeCacheInterfacePtr cache = getFrame(frame);

      if (!cache)
      {
        // There will be no cache for the very root of the tree
        break;
      }

      P_TimeAndFrameID latest = cache->getLatestTimeAndParent();

      if (latest.second == 0)
      {
        // Just break out here... there may still be a path from source -> target
        break;
      }

      if (!latest.first.isZero())
      {
        common_time = std::min(latest.first, common_time);
      }

      lct_cache.push_back(latest);

      frame = latest.second;

      // Early out... target frame is a direct parent of the source frame
      if (frame == target_id)
      {
        time = common_time;
        if (time == ros::TIME_MAX)
        {
          time = ros::Time();
        }
        return tf2_msgs::TF2Error::NO_ERROR;
      }

      ++depth;
      if (depth > MAX_GRAPH_DEPTH)
      {
        if (error_string)
        {
          std::stringstream ss;
          ss<<"The tf tree is invalid because it contains a loop." << std::endl
            << allFramesAsStringNoLock() << std::endl;
          *error_string = ss.str();
        }
        return tf2_msgs::TF2Error::LOOKUP_ERROR;
      }
    }

    // Now walk to the top parent from the target frame, accumulating the latest time and looking for a common parent
    frame = target_id;
    depth = 0;
    common_time = ros::TIME_MAX;
    tf2::CompactFrameID common_parent = 0;
    while (true)
    {
      TimeCacheInterfacePtr cache = getFrame(frame);

      if (!cache)
      {
        break;
      }

      P_TimeAndFrameID latest = cache->getLatestTimeAndParent();

      if (latest.second == 0)
      {
        break;
      }

      if (!latest.first.isZero())
      {
        common_time = std::min(latest.first, common_time);
      }

      std::vector<P_TimeAndFrameID>::iterator it = std::find_if(lct_cache.begin(), lct_cache.end(), TimeAndFrameIDFrameComparator(latest.second));
      if (it != lct_cache.end()) // found a common parent
      {
        common_parent = it->second;
        break;
      }

      frame = latest.second;

      // Early out... source frame is a direct parent of the target frame
      if (frame == source_id)
      {
        time = common_time;
        if (time == ros::TIME_MAX)
        {
          time = ros::Time();
        }
        return tf2_msgs::TF2Error::NO_ERROR;
      }

      ++depth;
      if (depth > MAX_GRAPH_DEPTH)
      {
        if (error_string)
        {
          std::stringstream ss;
          ss<<"The tf tree is invalid because it contains a loop." << std::endl
            << allFramesAsStringNoLock() << std::endl;
          *error_string = ss.str();
        }
        return tf2_msgs::TF2Error::LOOKUP_ERROR;
      }
    }

    if (common_parent == 0)
    {
      createConnectivityErrorString(source_id, target_id, error_string);
      return tf2_msgs::TF2Error::CONNECTIVITY_ERROR;
    }

    // Loop through the source -> root list until we hit the common parent
    {
      std::vector<P_TimeAndFrameID>::iterator it = lct_cache.begin();
      std::vector<P_TimeAndFrameID>::iterator end = lct_cache.end();
      for (; it != end; ++it)
      {
        if (!it->first.isZero())
        {
          common_time = std::min(common_time, it->first);
        }

        if (it->second == common_parent)
        {
          break;
        }
      }
    }

    if (common_time == ros::TIME_MAX)
    {
      common_time = ros::Time();
    }

    time = common_time;
    return tf2_msgs::TF2Error::NO_ERROR;
  }

  std::string OldBufferCore::allFramesAsYAML(double current_time) const
  {
    std::stringstream mstream;
    boost::mutex::scoped_lock lock(frame_mutex_);

    tf2::TransformStorage temp;

    if (frames_.size() ==1)
      mstream <<"{}";

    mstream.precision(3);
    mstream.setf(std::ios::fixed,std::ios::floatfield);

    //  for (std::vector< TimeCache*>::iterator  it = frames_.begin(); it != frames_.end(); ++it)
    for (unsigned int counter = 1; counter < frames_.size(); counter ++)//one referenced for 0 is no frame
    {
      auto cfid = tf2::CompactFrameID(counter);
      tf2::CompactFrameID frame_id_num;
      TimeCacheInterfacePtr cache = getFrame(cfid);
      if (!cache)
      {
        continue;
      }

      if(!cache->getData(ros::Time(), temp))
      {
        continue;
      }

      frame_id_num = temp.frame_id_;

      std::string authority = "no recorded authority";
      std::map<tf2::CompactFrameID, std::string>::const_iterator it = frame_authority_.find(cfid);
      if (it != frame_authority_.end()) {
        authority = it->second;
      }

      double rate = cache->getListLength() / std::max((cache->getLatestTimestamp().toSec() -
                                                       cache->getOldestTimestamp().toSec() ), 0.0001);

      mstream << std::fixed; //fixed point notation
      mstream.precision(3); //3 decimal places
      mstream << frameIDs_reverse[cfid] << ": " << std::endl;
      mstream << "  parent: '" << frameIDs_reverse[frame_id_num] << "'" << std::endl;
      mstream << "  broadcaster: '" << authority << "'" << std::endl;
      mstream << "  rate: " << rate << std::endl;
      mstream << "  most_recent_transform: " << (cache->getLatestTimestamp()).toSec() << std::endl;
      mstream << "  oldest_transform: " << (cache->getOldestTimestamp()).toSec() << std::endl;
      if ( current_time > 0 ) {
        mstream << "  transform_delay: " << current_time - cache->getLatestTimestamp().toSec() << std::endl;
      }
      mstream << "  buffer_length: " << (cache->getLatestTimestamp() - cache->getOldestTimestamp()).toSec() << std::endl;
    }

    return mstream.str();
  }

  std::string OldBufferCore::allFramesAsYAML() const
  {
    return this->allFramesAsYAML(0.0);
  }

  TransformableCallbackHandle OldBufferCore::addTransformableCallback(const TransformableCallback& cb)
  {
    boost::mutex::scoped_lock lock(transformable_callbacks_mutex_);
    TransformableCallbackHandle handle = ++transformable_callbacks_counter_;
    while (!transformable_callbacks_.insert(std::make_pair(handle, cb)).second)
    {
      handle = ++transformable_callbacks_counter_;
    }

    return handle;
  }

  struct OldBufferCore::RemoveRequestByCallback
  {
    RemoveRequestByCallback(TransformableCallbackHandle handle)
      : handle_(handle)
    {}

    bool operator()(const TransformableRequest& req)
    {
      return req.cb_handle == handle_;
    }

    TransformableCallbackHandle handle_;
  };

  void OldBufferCore::removeTransformableCallback(TransformableCallbackHandle handle)
  {
    {
      boost::mutex::scoped_lock lock(transformable_callbacks_mutex_);
      transformable_callbacks_.erase(handle);
    }

    {
      boost::mutex::scoped_lock lock(transformable_requests_mutex_);
      V_TransformableRequest::iterator it = std::remove_if(transformable_requests_.begin(), transformable_requests_.end(), RemoveRequestByCallback(handle));
      transformable_requests_.erase(it, transformable_requests_.end());
    }
  }

  TransformableRequestHandle OldBufferCore::addTransformableRequest(TransformableCallbackHandle handle, const std::string& target_frame, const std::string& source_frame, ros::Time time)
  {
    // shortcut if target == source
    if (target_frame == source_frame)
    {
      return 0;
    }

    TransformableRequest req;
    req.target_id = lookupFrameNumber(target_frame);
    req.source_id = lookupFrameNumber(source_frame);

    // First check if the request is already transformable.  If it is, return immediately
    if (canTransformInternal(req.target_id, req.source_id, time, 0))
    {
      return 0;
    }

    // Might not be transformable at all, ever (if it's too far in the past)
    if (req.target_id && req.source_id)
    {
      ros::Time latest_time;
      // TODO: This is incorrect, but better than nothing.  Really we want the latest time for
      // any of the frames
      getLatestCommonTime(req.target_id, req.source_id, latest_time, 0);
      if (!latest_time.isZero() && time + cache_time_ < latest_time)
      {
        return 0xffffffffffffffffULL;
      }
    }

    req.cb_handle = handle;
    req.time = time;
    req.request_handle = ++transformable_requests_counter_;
    if (req.request_handle == 0 || req.request_handle == 0xffffffffffffffffULL)
    {
      req.request_handle = 1;
    }

    if (req.target_id == 0)
    {
      req.target_string = target_frame;
    }

    if (req.source_id == 0)
    {
      req.source_string = source_frame;
    }

    boost::mutex::scoped_lock lock(transformable_requests_mutex_);
    transformable_requests_.push_back(req);

    return req.request_handle;
  }

  struct OldBufferCore::RemoveRequestByID
  {
    RemoveRequestByID(TransformableRequestHandle handle)
      : handle_(handle)
    {}

    bool operator()(const TransformableRequest& req)
    {
      return req.request_handle == handle_;
    }

    TransformableCallbackHandle handle_;
  };

  void OldBufferCore::cancelTransformableRequest(TransformableRequestHandle handle)
  {
    boost::mutex::scoped_lock lock(transformable_requests_mutex_);
    V_TransformableRequest::iterator it = std::remove_if(transformable_requests_.begin(), transformable_requests_.end(), RemoveRequestByID(handle));

    if (it != transformable_requests_.end())
    {
      transformable_requests_.erase(it, transformable_requests_.end());
    }
  }



// backwards compability for tf methods
  boost::signals2::connection OldBufferCore::_addTransformsChangedListener(boost::function<void(void)> callback)
  {
    boost::mutex::scoped_lock lock(transformable_requests_mutex_);
    return _transforms_changed_.connect(callback);
  }

  void OldBufferCore::_removeTransformsChangedListener(boost::signals2::connection c)
  {
    boost::mutex::scoped_lock lock(transformable_requests_mutex_);
    c.disconnect();
  }


  bool OldBufferCore::_frameExists(const std::string& frame_id_str) const
  {
    boost::mutex::scoped_lock lock(frame_mutex_);
    return frameIDs_.count(frame_id_str);
  }

  bool OldBufferCore::_getParent(const std::string& frame_id, ros::Time time, std::string& parent) const
  {

    boost::mutex::scoped_lock lock(frame_mutex_);
    tf2::CompactFrameID frame_number = lookupFrameNumber(frame_id);
    TimeCacheInterfacePtr frame = getFrame(frame_number);

    if (! frame)
      return false;

    tf2::CompactFrameID parent_id = frame->getParent(time, NULL);
    if (parent_id == 0)
      return false;

    parent = lookupFrameString(parent_id);
    return true;
  };

  void OldBufferCore::_getFrameStrings(std::vector<std::string> & vec) const
  {
    vec.clear();

    boost::mutex::scoped_lock lock(frame_mutex_);

    tf2::TransformStorage temp;

    //  for (std::vector< TimeCache*>::iterator  it = frames_.begin(); it != frames_.end(); ++it)
    for (unsigned int counter = 1; counter < frameIDs_reverse.size(); counter ++)
    {
      vec.push_back(frameIDs_reverse[counter]);
    }
    return;
  }




  void OldBufferCore::testTransformableRequests()
  {
    boost::mutex::scoped_lock lock(transformable_requests_mutex_);
    V_TransformableRequest::iterator it = transformable_requests_.begin();

    typedef boost::tuple<TransformableCallback&, TransformableRequestHandle, std::string,
      std::string, ros::Time&, TransformableResult&> TransformableTuple;
    std::vector<TransformableTuple> transformables;

    for (; it != transformable_requests_.end();)
    {
      TransformableRequest& req = *it;

      // One or both of the frames may not have existed when the request was originally made.
      if (req.target_id == 0)
      {
        req.target_id = lookupFrameNumber(req.target_string);
      }

      if (req.source_id == 0)
      {
        req.source_id = lookupFrameNumber(req.source_string);
      }

      ros::Time latest_time;
      bool do_cb = false;
      TransformableResult result = TransformAvailable;
      // TODO: This is incorrect, but better than nothing.  Really we want the latest time for
      // any of the frames
      getLatestCommonTime(req.target_id, req.source_id, latest_time, 0);
      if (!latest_time.isZero() && req.time + cache_time_ < latest_time)
      {
        do_cb = true;
        result = TransformFailure;
      }
      else if (canTransformInternal(req.target_id, req.source_id, req.time, 0))
      {
        do_cb = true;
        result = TransformAvailable;
      }

      if (do_cb)
      {
        {
          boost::mutex::scoped_lock lock2(transformable_callbacks_mutex_);
          M_TransformableCallback::iterator it = transformable_callbacks_.find(req.cb_handle);
          if (it != transformable_callbacks_.end())
          {
            transformables.push_back(boost::make_tuple(boost::ref(it->second),
                                                       req.request_handle,
                                                       lookupFrameString(req.target_id),
                                                       lookupFrameString(req.source_id),
                                                       boost::ref(req.time),
                                                       boost::ref(result)));
          }
        }

        if (transformable_requests_.size() > 1)
        {
          transformable_requests_[it - transformable_requests_.begin()] = transformable_requests_.back();
        }

        transformable_requests_.erase(transformable_requests_.end() - 1);
      }
      else
      {
        ++it;
      }
    }

    // unlock before allowing possible user callbacks to avoid potential deadlock (#91)
    lock.unlock();

    BOOST_FOREACH (TransformableTuple tt, transformables)
          {
            tt.get<0>()(tt.get<1>(), tt.get<2>(), tt.get<3>(), tt.get<4>(), tt.get<5>());
          }

    // Backwards compatability callback for tf
    _transforms_changed_();
  }


  std::string OldBufferCore::_allFramesAsDot(double current_time) const
  {
    std::stringstream mstream;
    mstream << "digraph G {" << std::endl;
    boost::mutex::scoped_lock lock(frame_mutex_);

    tf2::TransformStorage temp;

    if (frames_.size() == 1) {
      mstream <<"\"no tf data recieved\"";
    }
    mstream.precision(3);
    mstream.setf(std::ios::fixed,std::ios::floatfield);

    for (unsigned int counter = 1; counter < frames_.size(); counter ++) // one referenced for 0 is no frame
    {
      unsigned int frame_id_num;
      TimeCacheInterfacePtr counter_frame = getFrame(counter);
      if (!counter_frame) {
        continue;
      }
      if(!counter_frame->getData(ros::Time(), temp)) {
        continue;
      } else {
        frame_id_num = temp.frame_id_;
      }
      std::string authority = "no recorded authority";
      std::map<unsigned int, std::string>::const_iterator it = frame_authority_.find(counter);
      if (it != frame_authority_.end())
        authority = it->second;

      double rate = counter_frame->getListLength() / std::max((counter_frame->getLatestTimestamp().toSec() -
                                                               counter_frame->getOldestTimestamp().toSec()), 0.0001);

      mstream << std::fixed; //fixed point notation
      mstream.precision(3); //3 decimal places
      mstream << "\"" << frameIDs_reverse[frame_id_num] << "\"" << " -> "
              << "\"" << frameIDs_reverse[counter] << "\"" << "[label=\""
              //<< "Time: " << current_time.toSec() << "\\n"
              << "Broadcaster: " << authority << "\\n"
              << "Average rate: " << rate << " Hz\\n"
              << "Most recent transform: " << (counter_frame->getLatestTimestamp()).toSec() <<" ";
      if (current_time > 0)
        mstream << "( "<<  current_time - counter_frame->getLatestTimestamp().toSec() << " sec old)";
      mstream << "\\n"
              //    << "(time: " << getFrame(counter)->getLatestTimestamp().toSec() << ")\\n"
              //    << "Oldest transform: " << (current_time - getFrame(counter)->getOldestTimestamp()).toSec() << " sec old \\n"
              //    << "(time: " << (getFrame(counter)->getOldestTimestamp()).toSec() << ")\\n"
              << "Buffer length: " << (counter_frame->getLatestTimestamp()-counter_frame->getOldestTimestamp()).toSec() << " sec\\n"
              <<"\"];" <<std::endl;
    }

    for (unsigned int counter = 1; counter < frames_.size(); counter ++)//one referenced for 0 is no frame
    {
      unsigned int frame_id_num;
      TimeCacheInterfacePtr counter_frame = getFrame(counter);
      if (!counter_frame) {
        if (current_time > 0) {
          mstream << "edge [style=invis];" <<std::endl;
          mstream << " subgraph cluster_legend { style=bold; color=black; label =\"view_frames Result\";\n"
                  << "\"Recorded at time: " << current_time << "\"[ shape=plaintext ] ;\n "
                  << "}" << "->" << "\"" << frameIDs_reverse[counter] << "\";" << std::endl;
        }
        continue;
      }
      if (counter_frame->getData(ros::Time(), temp)) {
        frame_id_num = temp.frame_id_;
      } else {
        frame_id_num = 0;
      }

      if(frameIDs_reverse[frame_id_num]=="NO_PARENT")
      {
        mstream << "edge [style=invis];" <<std::endl;
        mstream << " subgraph cluster_legend { style=bold; color=black; label =\"view_frames Result\";\n";
        if (current_time > 0)
          mstream << "\"Recorded at time: " << current_time << "\"[ shape=plaintext ] ;\n ";
        mstream << "}" << "->" << "\"" << frameIDs_reverse[counter] << "\";" << std::endl;
      }
    }
    mstream << "}";
    return mstream.str();
  }

  std::string OldBufferCore::_allFramesAsDot() const
  {
    return _allFramesAsDot(0.0);
  }

  void OldBufferCore::_chainAsVector(const std::string & target_frame, ros::Time target_time, const std::string & source_frame, ros::Time source_time, const std::string& fixed_frame, std::vector<std::string>& output) const
  {
    std::string error_string;

    output.clear(); //empty vector

    std::stringstream mstream;
    boost::mutex::scoped_lock lock(frame_mutex_);

    TransformAccum accum;

    // Get source frame/time using getFrame
    tf2::CompactFrameID source_id = lookupFrameNumber(source_frame);
    tf2::CompactFrameID fixed_id = lookupFrameNumber(fixed_frame);
    tf2::CompactFrameID target_id = lookupFrameNumber(target_frame);

    std::vector<tf2::CompactFrameID> source_frame_chain;
    int retval = walkToTopParent(accum, source_time, fixed_id, source_id, &error_string, &source_frame_chain);

    if (retval != tf2_msgs::TF2Error::NO_ERROR)
    {
      switch (retval)
      {
        case tf2_msgs::TF2Error::CONNECTIVITY_ERROR:
          throw tf2::ConnectivityException(error_string);
        case tf2_msgs::TF2Error::EXTRAPOLATION_ERROR:
          throw tf2::ExtrapolationException(error_string);
        case tf2_msgs::TF2Error::LOOKUP_ERROR:
          throw tf2::LookupException(error_string);
        default:
          CONSOLE_BRIDGE_logError("Unknown error code: %d", retval);
          assert(0);
      }
    }

    std::vector<tf2::CompactFrameID> target_frame_chain;
    retval = walkToTopParent(accum, target_time, target_id, fixed_id, &error_string, &target_frame_chain);

    if (retval != tf2_msgs::TF2Error::NO_ERROR)
    {
      switch (retval)
      {
        case tf2_msgs::TF2Error::CONNECTIVITY_ERROR:
          throw tf2::ConnectivityException(error_string);
        case tf2_msgs::TF2Error::EXTRAPOLATION_ERROR:
          throw tf2::ExtrapolationException(error_string);
        case tf2_msgs::TF2Error::LOOKUP_ERROR:
          throw tf2::LookupException(error_string);
        default:
          CONSOLE_BRIDGE_logError("Unknown error code: %d", retval);
          assert(0);
      }
    }
    // If the two chains overlap clear the overlap
    if (source_frame_chain.size() > 0 && target_frame_chain.size() > 0 &&
        source_frame_chain.back() == target_frame_chain.front())
    {
      source_frame_chain.pop_back();
    }
    // Join the two walks
    for (unsigned int i = 0; i < target_frame_chain.size(); ++i)
    {
      source_frame_chain.push_back(target_frame_chain[i]);
    }


    // Write each element of source_frame_chain as string
    for (unsigned int i = 0; i < source_frame_chain.size(); ++i)
    {
      output.push_back(lookupFrameString(source_frame_chain[i]));
    }
  }

  int TestOldBufferCore::_walkToTopParent(OldBufferCore& buffer, ros::Time time, tf2::CompactFrameID target_id, tf2::CompactFrameID source_id, std::string* error_string, std::vector<tf2::CompactFrameID> *frame_chain) const
  {
    TransformAccum accum;
    return buffer.walkToTopParent(accum, time, target_id, source_id, error_string, frame_chain);
  }

} // namespace tf2