#include <iostream>
#include <thread>
#include <geometry_msgs/TransformStamped.h>
#include <atomic>
#include <chrono>
#include <vector>
#include <gflags/gflags.h>
#include <cstdlib>
#include <console_bridge/console.h>
#include <bits/stdc++.h>
#include <fstream>
#include <limits>

#include "tf2/read_stat.h"
#include "../old_tf2/old_buffer_core.h"
#include "../silo_tf2/silo_buffer_core.h"
#include "tf2/buffer_core.h"
#include "xoroshiro128_plus.h"

using old_tf2::OldBufferCore;
using silo_tf2::SiloBufferCore;
using tf2::BufferCore;
using namespace geometry_msgs;
using namespace std;

DEFINE_uint64(thread, std::thread::hardware_concurrency(), "Thread size");
DEFINE_uint64(joint, 10, "Joint size");
DEFINE_double(read_ratio, 1, "read ratio, within [0,1]");
DEFINE_uint64(read_len, 4, "Number of reading joint size ∈ [0, joint]");
DEFINE_uint64(write_len, 4, "Number of writing joint size ∈ [0, joint]");
DEFINE_string(output, "/tmp/a.dat", "Output file");
DEFINE_uint32(only, 0, "0: All, 1: Only snapshot, 2: Only Latest, 3: except old, 4: Only old, 5: except snapshot, 6: Only Silo");
DEFINE_double(frequency, 0, "frequency, when 0 then disabled");
DEFINE_uint64(loop_sec, 10, "loop second");
DEFINE_bool(opposite_write_direction, true, "when true, opposite write direction");


using std::chrono::operator""s;
using std::chrono::duration_cast;

TransformStamped trans(
  const string &parent,
  const string &child,
  double time){
  TransformStamped tr{};
  tr.header.frame_id = parent;
  tr.child_frame_id = child;
  tr.header.stamp = ros::Time(time);
  tr.transform.rotation.w = 1;
  return tr;
}

template <typename T>
void make_snake(T &bfc){
  // link0 -- link1 -- .. -- link100
  auto now = chrono::steady_clock::now();
  double nano = chrono::duration<double>(now.time_since_epoch()).count();
  for(size_t i = 0; i < FLAGS_joint; i++){
    auto i_iplus1 = trans("link" + to_string(i),
                          "link" + to_string(i+1),
                          nano);
    bfc.setTransform(i_iplus1, "me");
  }
}

template <typename T>
struct BufferCoreWrapper{
  void init(){}
  void read(size_t link, size_t until, ReadStat &out_stat)const{}
  // return abort count
  void write(size_t link, size_t until, double nano_time, WriteStat &out_stat, size_t &iter_acc){}
};

template <>
struct BufferCoreWrapper<OldBufferCore>{
  OldBufferCore bfc{};
  void init(){
    bfc.clear();
    make_snake(bfc);
  }
  void read(size_t link, size_t until, ReadStat &out_stat) const{
    // Read from low to high for performance.
    assert(until > link);
    auto trans = bfc.lookupTransform("link" + to_string(link),
                                     "link" + to_string(until),
                                     ros::Time(0));
    out_stat.timestamps.push_back(trans.header.stamp.toNSec());
  }
  void write(size_t link, size_t until, double nano_time, WriteStat &out_stat, size_t &iter_acc){
    // Write from low to high for performance.
    assert(until > link);
    if(FLAGS_opposite_write_direction){
      for(size_t j = link; j < until; j++){
        bfc.setTransform(trans("link" + to_string(j),
                               "link" + to_string(j+1),
                               nano_time), "me");
        iter_acc++;
      }
    }else{
      for(size_t j = until; j > link; j--){
        bfc.setTransform(trans("link" + to_string(j-1),
                               "link" + to_string(j),
                               nano_time), "me");
        iter_acc++;
      }
    }
  }
};

enum AccessType{
  Snapshot, Latest
};

template <>
struct BufferCoreWrapper<BufferCore>{

  explicit BufferCoreWrapper(AccessType accessType_)
  : accessType(accessType_){}

  BufferCore bfc{};
  AccessType accessType;

  void init(){
    bfc.clear();
    make_snake(bfc);
  }
  void read(size_t link, size_t until, ReadStat &out_stat) const{
    if(accessType == Snapshot){
      assert(until > link);
      auto trans = bfc.lookupTransform("link" + to_string(link),
                                       "link" + to_string(until),
                                       ros::Time(0), &out_stat);
      out_stat.timestamps.push_back(trans.header.stamp.toNSec());
    }else{
      bfc.lookupLatestTransform("link" + to_string(link),
                                "link" + to_string(until), &out_stat);
    }
  }
  void write(size_t link, size_t until, double nano_time, WriteStat &out_stat, size_t &iter_acc){
    assert(until > link);
    if(accessType == Snapshot){
      if(FLAGS_opposite_write_direction){
        for(size_t j = link; j < until; j++){
          bfc.setTransform(trans("link" + to_string(j),
                                 "link" + to_string(j+1),
                                 nano_time), "me");
          iter_acc++;
        }
      }else{
        for(size_t j = until; j > link; j--){
          bfc.setTransform(trans("link" + to_string(j-1),
                                 "link" + to_string(j),
                                 nano_time), "me");
          iter_acc++;
        }
      }
    }else{
      // which write direction is proper?
      vector<TransformStamped> vec{};
//      for(size_t j = until; j > link; j--){
//        vec.push_back(trans("link" + to_string(j-1),
//                            "link" + to_string(j),
//                            nano_time));
//      }
      if(FLAGS_opposite_write_direction){
        for(size_t j = link; j < until; j++){
          vec.push_back(trans("link" + to_string(j),
                              "link" + to_string(j+1),
                              nano_time));
        }
      }else{
        for(size_t j = until; j > link; j--){
          vec.push_back(trans("link" + to_string(j-1),
                              "link" + to_string(j),
                              nano_time));
        }
      }

      bfc.setTransforms(vec, "me", false, &out_stat);
      iter_acc++;
    }
  }
};

template <>
struct BufferCoreWrapper<SiloBufferCore>{

  explicit BufferCoreWrapper(){}

  SiloBufferCore bfc{};

  void init(){
    make_snake(bfc);
  }
  void read(size_t link, size_t until, ReadStat &out_stat) const{
    assert(until > link);
    bfc.lookupLatestTransform("link" + to_string(link),
                              "link" + to_string(until), &out_stat);
  }
  void write(size_t link, size_t until, double nano_time, WriteStat &out_stat, size_t &iter_acc){
    assert(until > link);
    vector<TransformStamped> vec{};
    for(size_t j = link; j < until; j++){
      vec.push_back(trans("link" + to_string(j),
                          "link" + to_string(j+1),
                          nano_time));
    }
    bfc.setTransforms(vec, "me", false, &out_stat);
    iter_acc++;
  }
};


template <typename T>
T make_ave(const std::vector<T> &vec){
  // don't call this!
  assert(false);
}

template <>
double make_ave<double>(const std::vector<double> &vec){
  double tmp{};
  for(auto &e: vec) {
    tmp += e / (double) vec.size();
  }
  return tmp;
}

template <>
chrono::duration<double> make_ave<chrono::duration<double>>(const std::vector<chrono::duration<double>> &vec){
  chrono::duration<double> tmp{};
  for(auto &e: vec) {
    tmp += e / (double) vec.size();
  }
  return tmp;
}

template <typename T>
struct CountAccum{
  explicit CountAccum(size_t count): vec(count, T{}){}
  CountAccum(const CountAccum<T> &other) = delete;
  CountAccum(CountAccum<T> &&other) = delete;

  void record(size_t t_id, const T &data){
    vec[t_id] = data;
  };

  T average() const{
    return make_ave(vec);
  }

  T sum() const{
    return std::accumulate(vec.begin(), vec.end(), T{});
  }

  std::vector<T> vec;
};

struct RunResult{
  chrono::duration<double> time;
  double throughput;
  chrono::duration<double> readLatency;
  chrono::duration<double> writeLatency;
  chrono::duration<double> delay; // how far does took data from now
  // optional
  chrono::duration<double> var; // should be zero except latest
  double aborts; // aborts in write, should be zero expect latest
  double readAborts;

  double readThroughput;
  double writeThroughput;
};

double throughput(chrono::duration<double> time, size_t iter){
  return ((double) iter) * (1. / chrono::duration<double>(time).count());
}

// time, delay, latency, aborts, var
template <typename T>
RunResult run(BufferCoreWrapper<T> &bfc_w){
  auto read_threads = (size_t)std::round((double)FLAGS_thread * FLAGS_read_ratio);
  size_t write_threads = FLAGS_thread - read_threads;

  atomic_bool wait{true};
  vector<thread> threads{};

  CountAccum<double> throughput_acc_read_thread(read_threads);
  CountAccum<chrono::duration<double>> delay_acc_thread(read_threads);
  CountAccum<chrono::duration<double>> vars_acc_thread(read_threads);
  CountAccum<chrono::duration<double>> latencies_acc_read_thread(read_threads);
  CountAccum<double> read_wait_count(read_threads);
  CountAccum<double> deque_count_thread(read_threads);
  CountAccum<double> abort_acc_read_thread(read_threads);


  for(size_t t = 0; t < read_threads; t++){
    threads.emplace_back([t,&wait, &bfc_w, &delay_acc_thread,
                          &vars_acc_thread, &latencies_acc_read_thread,
                          &throughput_acc_read_thread, &read_wait_count,
                          &deque_count_thread,
                          &abort_acc_read_thread](){
      std::random_device rnd;
      Xoroshiro128Plus r(rnd());
      while (wait){;}
      chrono::duration<double> delay_iter_acc{}, var_iter_acc{}, latency_iter_acc{};
      size_t iter_count = 0;
      auto start_iter = chrono::steady_clock::now();
      auto end_iter = start_iter;
      size_t read_wait_count_acc{};
      size_t deque_count_acc{};
      uint64_t abort_iter_acc{};

      for(;;){
        ReadStat stat{};
        size_t link = r.next() % FLAGS_joint;
        auto until = link + FLAGS_read_len;
        if(until > FLAGS_joint) until = FLAGS_joint;
        auto before = chrono::steady_clock::now();
        bfc_w.read(link, until, stat);
        auto after = chrono::steady_clock::now();

        auto access_ave = operator""ns(stat.getTimeStampsAve());
//          assert(now.time_since_epoch() > access_ave);
        delay_iter_acc += before.time_since_epoch() - access_ave; // can be minus!
        var_iter_acc += operator""ns(stat.getTimeStampsStandardDiv());
        latency_iter_acc += after - before;
        read_wait_count_acc += stat.tryReadLockCount;
        deque_count_acc += stat.dequeSize;
        abort_iter_acc += stat.abortCount;

        if(FLAGS_frequency != 0){
          this_thread::sleep_for(operator""s((1. / FLAGS_frequency)));
        }

        iter_count++;

        end_iter = chrono::steady_clock::now();

        if(end_iter - start_iter > operator""s(FLAGS_loop_sec)){
          break;
        }
      }

      throughput_acc_read_thread.record(t, throughput(end_iter - start_iter, iter_count));
      delay_acc_thread.record(t,delay_iter_acc / (double) iter_count);
      vars_acc_thread.record(t, var_iter_acc / (double) iter_count);
      latencies_acc_read_thread.record(t, latency_iter_acc / (double) iter_count);
      read_wait_count.record(t, (double) read_wait_count_acc / (double) iter_count);
      deque_count_thread.record(t, (double) deque_count_acc / (double) iter_count);
      abort_acc_read_thread.record(t, (double) abort_iter_acc / (double) iter_count);
    });
  }

  CountAccum<double> abort_acc_thread(write_threads);
  CountAccum<double> throughput_acc_write_thread(write_threads);
  CountAccum<chrono::duration<double>> latencies_acc_write_thread(write_threads);

  for(size_t t = 0; t < write_threads; t++){
    threads.emplace_back([t, &bfc_w, &wait, &abort_acc_thread,
                          &throughput_acc_write_thread, &latencies_acc_write_thread, read_threads](){
      std::random_device rnd;
      Xoroshiro128Plus r(rnd());
      while (wait){;}
      uint64_t abort_iter_acc{};
      auto start_iter = chrono::steady_clock::now();
      auto end_iter = start_iter;
      size_t iter_count = 0;
      chrono::duration<double> latency_iter_acc{};

      for(;;){
        size_t link = r.next() % FLAGS_joint;
        auto until = link + FLAGS_write_len;
        if(until > FLAGS_joint) until = FLAGS_joint;
        vector<TransformStamped> vec{};
        auto before = chrono::steady_clock::now();
        double nano = chrono::duration<double>(before.time_since_epoch()).count(); // from sec
        WriteStat stat{};
        bfc_w.write(link, until, nano, stat, iter_count);
        auto after = chrono::steady_clock::now();
        abort_iter_acc += stat.getAbortCount();
        latency_iter_acc += after - before;

        if(FLAGS_frequency != 0){
          this_thread::sleep_for(operator""s((1. / FLAGS_frequency)));
        }

        end_iter = chrono::steady_clock::now();

        if(end_iter - start_iter > operator""s(FLAGS_loop_sec)){
          break;
        }
      }

      throughput_acc_write_thread.record(t, throughput(end_iter - start_iter, iter_count));
      abort_acc_thread.record(t, (double) abort_iter_acc / (double) iter_count);
      latencies_acc_write_thread.record(t, latency_iter_acc / (double) iter_count);
    });
  }

  bfc_w.init();
  for(size_t t = 0; t < threads.size(); t++){
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(t, &cpuset);
    int rc = pthread_setaffinity_np(threads[t].native_handle(),
                                    sizeof(cpu_set_t), &cpuset);
    if (rc != 0) {
      std::cerr << "Error calling pthread_setaffinity_np: " << rc << "\n";
      exit(-1);
    }
  }
  asm volatile("" ::: "memory"); // force not to reorder.
  auto start = chrono::high_resolution_clock::now();
  wait = false;
  for(auto &e: threads){
    e.join();
  }
  auto finish = chrono::high_resolution_clock::now();

  RunResult result{};
  result.time = finish - start;
  result.throughput = throughput_acc_read_thread.sum() + throughput_acc_write_thread.sum();
  result.readLatency = latencies_acc_read_thread.average();
  result.writeLatency = latencies_acc_write_thread.average();
  result.delay = delay_acc_thread.average();
  result.var = vars_acc_thread.average();
  result.aborts = abort_acc_thread.average();
  result.readThroughput = throughput_acc_read_thread.sum();
  result.writeThroughput = throughput_acc_write_thread.sum();
  result.readAborts = abort_acc_read_thread.average();

  cout << "read wait count: " << read_wait_count.average() << endl;
  cout << "deque size in snapshot: " << deque_count_thread.average() << endl;

  return result;
}

int main(int argc, char* argv[]){
  gflags::SetUsageMessage("speed check");
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  console_bridge::setLogLevel(console_bridge::LogLevel::CONSOLE_BRIDGE_LOG_INFO);

  CONSOLE_BRIDGE_logInform("thread: %d", FLAGS_thread);
  CONSOLE_BRIDGE_logInform("joint: %d", FLAGS_joint);
  CONSOLE_BRIDGE_logInform("read ratio: %lf", FLAGS_read_ratio);
  if(!(0. <= FLAGS_read_ratio and FLAGS_read_ratio <= 1.)){
    CONSOLE_BRIDGE_logError("wrong read ratio");
    exit(-1);
  }
  CONSOLE_BRIDGE_logInform("read len: %d", FLAGS_read_len);
  if(!(0 <= FLAGS_read_len and FLAGS_read_len <= FLAGS_joint)){
    CONSOLE_BRIDGE_logError("wrong read len");
    exit(-1);
  }
  CONSOLE_BRIDGE_logInform("write len: %d", FLAGS_write_len);
  if(!(0 <= FLAGS_write_len and FLAGS_write_len <= FLAGS_joint)){
    CONSOLE_BRIDGE_logError("wrong write len");
    exit(-1);
  }
  CONSOLE_BRIDGE_logInform("Output: %s", FLAGS_output.c_str());
  CONSOLE_BRIDGE_logInform("Only: %d", FLAGS_only);
  CONSOLE_BRIDGE_logInform("frequency: %lf", FLAGS_frequency);
  CONSOLE_BRIDGE_logInform("Opposite write direction: %s", FLAGS_opposite_write_direction ? "true" : "false");

  console_bridge::setLogLevel(console_bridge::LogLevel::CONSOLE_BRIDGE_LOG_ERROR);

  ofstream output{};
  output.open(FLAGS_output.c_str(), std::ios_base::app);

  cout << std::setprecision(std::numeric_limits<double>::digits10);

  RunResult old_result{};
  if(FLAGS_only == 0 or FLAGS_only == 4 or FLAGS_only == 5){
    BufferCoreWrapper<OldBufferCore> bfc_w{};
    old_result = run(bfc_w);

    cout << "old: " << endl;
    cout << "\t" << "time: " << chrono::duration<double, std::milli>(old_result.time).count() << "ms" << endl;
    cout << "\t" << "throughput: " << old_result.throughput << endl;
    cout << "\t" << "read latency: " << chrono::duration<double, std::milli>(old_result.readLatency).count() << "ms" << endl;
    cout << "\t" << "write latency: " << chrono::duration<double, std::milli>(old_result.writeLatency).count() << "ms" << endl;
    cout << "\t" << "delay: " << chrono::duration<double, std::milli>(old_result.delay).count() << "ms" << endl;
  }

  RunResult snapshot_result{};
  if(FLAGS_only == 0 or FLAGS_only == 1 or FLAGS_only == 3){
    BufferCoreWrapper<BufferCore> bfc_w(AccessType::Snapshot);
    snapshot_result = run(bfc_w);

    cout << "snapshot:" << endl;
    cout << "\t" << "time: " << chrono::duration<double, std::milli>(snapshot_result.time).count() << "ms" << endl;
    cout << "\t" << "throughput: " << snapshot_result.throughput << endl;
    cout << "\t" << "read latency: " << chrono::duration<double, std::milli>(snapshot_result.readLatency).count() << "ms" << endl;
    cout << "\t" << "write latency: " << chrono::duration<double, std::milli>(snapshot_result.writeLatency).count() << "ms" << endl;
    cout << "\t" << "delay: " << chrono::duration<double, std::milli>(snapshot_result.delay).count() << "ms" << endl;

  }

  RunResult latest_result{};
  if(FLAGS_only == 0 or FLAGS_only == 2 or FLAGS_only == 3 or FLAGS_only == 5){
    BufferCoreWrapper<BufferCore> bfc_w(AccessType::Latest);
    latest_result = run(bfc_w);

    cout << "latest:" << endl;
    cout << "\t" << "time: " << chrono::duration<double, std::milli>(latest_result.time).count() << "ms" << endl;
    cout << "\t" << "throughput: " << latest_result.throughput << endl;
    cout << "\t" << "read latency: " << chrono::duration<double, std::milli>(latest_result.readLatency).count() << "ms" << endl;
    cout << "\t" << "write latency: " << chrono::duration<double, std::milli>(latest_result.writeLatency).count() << "ms" << endl;
    cout << "\t" << "delay: " << chrono::duration<double, std::milli>(latest_result.delay).count() << "ms" << endl;
    cout << "\t" << "var: " << chrono::duration<double, std::milli>(latest_result.var).count() << "ms" << endl;
    cout << "\t" << "aborts: " << latest_result.aborts << " times" << endl;

  }

  RunResult silo_result{};
  if(FLAGS_only == 0 or FLAGS_only == 6){
    BufferCoreWrapper<SiloBufferCore> bfc_w{};
    silo_result = run(bfc_w);

    cout << "Silo:" << endl;
    cout << "\t" << "time: " << chrono::duration<double, std::milli>(silo_result.time).count() << "ms" << endl;
    cout << "\t" << "throughput: " << silo_result.throughput << endl;
    cout << "\t" << "read latency: " << chrono::duration<double, std::milli>(silo_result.readLatency).count() << "ms" << endl;
    cout << "\t" << "write latency: " << chrono::duration<double, std::milli>(silo_result.writeLatency).count() << "ms" << endl;
    cout << "\t" << "delay: " << chrono::duration<double, std::milli>(silo_result.delay).count() << "ms" << endl;
    cout << "\t" << "var: " << chrono::duration<double, std::milli>(silo_result.var).count() << "ms" << endl;
    cout << "\t" << "read aborts: " << silo_result.readAborts << " times" << endl;
  }

  if(FLAGS_frequency != 0){
    cout << "\033[31mWarn: frequency defined, so throughput is not making any sense!\033[0m" << endl;
  }

  output << FLAGS_thread << " "; // 1
  output << FLAGS_joint << " "; // 2
  output << FLAGS_read_ratio << " "; // 3
  output << FLAGS_read_len << " "; // 4
  output << FLAGS_write_len << " "; // 5
  output << FLAGS_frequency << " "; // 6
  output << old_result.throughput << " "; // 7
  output << snapshot_result.throughput << " "; // 8
  output << latest_result.throughput << " "; // 9
  output << latest_result.aborts << " "; // 10
  output << chrono::duration<double, std::milli>(old_result.readLatency).count() << " "; // 11
  output << chrono::duration<double, std::milli>(snapshot_result.readLatency).count() << " "; // 12
  output << chrono::duration<double, std::milli>(latest_result.readLatency).count() << " "; // 13
  output << chrono::duration<double, std::milli>(old_result.delay).count() << " "; // 14
  output << chrono::duration<double, std::milli>(snapshot_result.delay).count() << " "; // 15
  output << chrono::duration<double, std::milli>(latest_result.delay).count() << " "; // 16
  output << chrono::duration<double, std::milli>(latest_result.var).count() << " "; // 17
  output << chrono::duration<double, std::milli>(old_result.writeLatency).count() << " "; // 18
  output << chrono::duration<double, std::milli>(snapshot_result.writeLatency).count() << " "; // 19
  output << chrono::duration<double, std::milli>(latest_result.writeLatency).count() << " "; // 20
  output << old_result.readThroughput << " "; // 21
  output << snapshot_result.readThroughput << " "; // 22
  output << latest_result.readThroughput << " "; // 23
  output << old_result.writeThroughput << " "; // 24
  output << snapshot_result.writeThroughput << " "; // 25
  output << latest_result.writeThroughput << " "; // 26
  output << (FLAGS_opposite_write_direction ? "opposite" : "direct") << " "; // 27
  output << silo_result.throughput << " "; // 28
  output << silo_result.readAborts << " "; // 29
  output << chrono::duration<double, std::milli>(silo_result.readLatency).count() << " "; // 30
  output << chrono::duration<double, std::milli>(silo_result.writeLatency).count() << " "; // 31
  output << chrono::duration<double, std::milli>(silo_result.delay).count() << " "; // 32

  output << endl;
  output.close();

  return 0;
}