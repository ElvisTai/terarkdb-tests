//
// Created by terark on 16-8-2.
//

#ifndef TERARKDB_TEST_FRAMEWORK_LEVELDB_H
#define TERARKDB_TEST_FRAMEWORK_LEVELDB_H
#include <sys/types.h>
#include <sys/time.h>
#include <sstream>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include "db/db_impl.h"
#include "db/version_set.h"
#include "include/leveldb/cache.h"
#include "include/leveldb/db.h"
#include "include/leveldb/env.h"
#include "include/leveldb/write_batch.h"
#include "port/port.h"
#include "util/crc32c.h"
#include "util/histogram.h"
#include "util/mutexlock.h"
#include "util/random.h"
#include "util/testutil.h"

#include <iostream>
#include <fstream>
#include <string.h>
#include <string>

//#include "stdafx.h"
#include <terark/db/db_table.hpp>
#include <terark/io/MemStream.hpp>
#include <terark/io/DataIO.hpp>
#include <terark/io/RangeStream.hpp>
#include <terark/lcast.hpp>
#include <terark/util/autofree.hpp>
#include <terark/util/fstrvec.hpp>
#include <port/port_posix.h>
#include <src/Setting.h>
#include <wiredtiger.h>
#include <chrono>
#include <thread>
#include <mutex>

using namespace terark;
using namespace db;

namespace leveldb {

    namespace {

        static Slice TrimSpace(Slice s) {
            int start = 0;
            while (start < s.size() && isspace(s[start])) {
                start++;
            }
            int limit = s.size();
            while (limit > start && isspace(s[limit-1])) {
                limit--;
            }
            return Slice(s.data() + start, limit - start);
        }

        static void AppendWithSpace(std::string* str, Slice msg) {
            if (msg.empty()) return;
            if (!str->empty()) {
                str->push_back(' ');
            }
            str->append(msg.data(), msg.size());
        }

        class Stats {

        private:
            double start_;
            double finish_;
            double seconds_;
            int done_;
            int next_report_;
            int64_t bytes_;
            double last_op_finish_;
            Histogram hist_;
            std::string message_;
            Setting &setting;

            static std::time_t startTime;
            static std::time_t endTime;

            static std::atomic<uint64_t > totalRead;
            static std::atomic<uint64_t > totalWrite;
            std::atomic<uint64_t >* totalDone_[0];
        public:
            std::atomic<uint64_t > typedDone_[2];//0:write 1:read
            Stats(Setting &setting1) :setting(setting1) {

                typedDone_[0].store(0);
                typedDone_[1].store(0);

                totalDone_[0] = &totalWrite;
                totalDone_[1] = &totalRead;
                Start();
            }

            void Start() {
                next_report_ = 100;
                last_op_finish_ = start_;
                hist_.Clear();
                done_ = 0;
                bytes_ = 0;
                seconds_ = 0;
                start_ = Env::Default()->NowMicros();
                finish_ = start_;
                message_.clear();
            }

            void Merge(const Stats& other) {
                hist_.Merge(other.hist_);
                done_ += other.done_;
                bytes_ += other.bytes_;
                seconds_ += other.seconds_;
                if (other.start_ < start_) start_ = other.start_;
                if (other.finish_ > finish_) finish_ = other.finish_;

                // Just keep the messages from one thread
                if (message_.empty()) message_ = other.message_;
            }

            void Stop() {
                finish_ = Env::Default()->NowMicros();
                seconds_ = (finish_ - start_) * 1e-6;
            }

            void AddMessage(Slice msg) {
                AppendWithSpace(&message_, msg);
            }

            void FinishedSingleOp() {
                if (setting.FLAGS_histogram) {
                    double now = Env::Default()->NowMicros();
                    double micros = now - last_op_finish_;
                    hist_.Add(micros);
                    if (micros > 20000) {
                        fprintf(stderr, "long op: %.1f micros%30s\r", micros, "");
                        fflush(stderr);
                    }
                    last_op_finish_ = now;
                }

                done_++;
                if (done_ >= next_report_) {
                    if      (next_report_ < 1000)   next_report_ += 100;
                    else if (next_report_ < 5000)   next_report_ += 500;
                    else if (next_report_ < 10000)  next_report_ += 1000;
                    else if (next_report_ < 50000)  next_report_ += 5000;
                    else if (next_report_ < 100000) next_report_ += 10000;
                    else if (next_report_ < 500000) next_report_ += 50000;
                    else                            next_report_ += 100000;
                    fprintf(stderr, "... finished %d ops%30s\r", done_, "");
                    fflush(stderr);
                }
            }
            void FinishedSingleOp(unsigned char type){
                typedDone_[type]++;
                if ( type == 0)
                    totalWrite++;
                else
                    totalRead++;
            }
            static bool timeInit( void){
                //此函数由于会修改本类中的静态变量，所以同一时间只能允许一个线程访问。
                static std::timed_mutex time_mtx;

                if ( false == time_mtx.try_lock_for(std::chrono::milliseconds(100)))
                    return false;

                startTime = time(NULL);
                time_mtx.unlock();
                return true;
            }
            static std::string GetOps( void){
                //此函数由于会修改本类中的静态变量，所以同一时间只能允许一个线程访问。
                static std::timed_mutex time_mtx;

                if ( false == time_mtx.try_lock_for(std::chrono::milliseconds(100)))
                    return std::string("Can't access now!");

                uint64_t writeOps = Stats::totalWrite.exchange(0);
                uint64_t readOps = Stats::totalRead.exchange(0);
                endTime = time(NULL);
                std::stringstream opsInfo;
                opsInfo <<"Start:" << ctime(&startTime);
                startTime = endTime;
                opsInfo <<"\tEnd:" << ctime(&endTime);
                opsInfo <<"\tread OPS:" << readOps;
                opsInfo <<"\twrite OPS:" << writeOps << std::endl;
                time_mtx.unlock();

                return opsInfo.str();
            }
            void AddBytes(int64_t n) {
                bytes_ += n;
            }

            void Report(const Slice& name) {
                // Pretend at least one op was done in case we are running a benchmark
                // that does not call FinishedSingleOp().
                if (done_ < 1) done_ = 1;

                std::string extra;
                if (bytes_ > 0) {
                    // Rate is computed on actual elapsed time, not the sum of per-thread
                    // elapsed times.
                    double elapsed = (finish_ - start_) * 1e-6;
                    char rate[100];
                    snprintf(rate, sizeof(rate), "%6.1f MB/s",
                             (bytes_ / 1048576.0) / elapsed);
                    extra = rate;
                }
                AppendWithSpace(&extra, message_);

                fprintf(stdout, "%-12s : %11.3f micros/op;%s%s\n",
                        name.ToString().c_str(),
                        seconds_ * 1e6 / done_,
                        (extra.empty() ? "" : " "),
                        extra.c_str());
                if (setting.FLAGS_histogram) {
                    fprintf(stdout, "Microseconds per op:\n%s\n", hist_.ToString().c_str());
                }
                fflush(stdout);
            }
        };

// State shared by all concurrent executions of the same benchmark.
        struct SharedState {
            port::Mutex mu;
            port::CondVar cv;
            int total;
            Setting *setting;
            // Each thread goes through the following states:
            //    (1) initializing
            //    (2) waiting for others to be initialized
            //    (3) running
            //    (4) done

            int num_initialized;
            int num_done;
            bool start;
            SharedState() : cv(&mu) { }
        };

// Per-thread state for concurrent executions of the same benchmark.
        struct ThreadState {
            int tid;             // 0..n-1 when running in n threads
            Random rand;         // Has different seeds for different threads
            Stats *stats;
            SharedState* shared;
            std::atomic<uint8_t> STOP;
            WT_SESSION *session;
            std::atomic<std::vector<uint8_t >*> *which;
            ThreadState(int index,Setting &setting1,std::atomic<std::vector<uint8_t >*>* w)
                    :   tid(index),
                        rand(1000 + index),
                        which(w)
                        {
                stats = new Stats(setting1);
                STOP.store(false);
            }
            ThreadState(int index,Setting &setting1,WT_CONNECTION *conn,std::atomic<std::vector<uint8_t >*>* w)
            :tid(index),rand(index + 1000),which(w){

                stats = new Stats(setting1);
                conn->open_session(conn, NULL, NULL, &session);
                STOP.store(false);
                assert(session != NULL);
            }
            ~ThreadState(){
                delete(stats);
            }
        };

        struct TestRow {
            std::string product_userId;
            std::string profileName;
            uint32_t helpfulness1;
            uint32_t helpfulness2;
            uint32_t score;
            uint32_t time;
            std::string summary;
            std::string text;

            DATA_IO_LOAD_SAVE(TestRow,
                    &Schema::StrZero(product_userId)
                    &Schema::StrZero(profileName)
            &helpfulness1
                    &helpfulness2
            &score
                    &time
            &Schema::StrZero(summary)
            &Schema::StrZero(text)
            )
        };


    }  // namespace

    std::atomic<uint64_t >leveldb::Stats::totalRead(0);
    std::atomic<uint64_t >leveldb::Stats::totalWrite(0);
    std::time_t leveldb::Stats::startTime = time(NULL);
    std::time_t leveldb::Stats::endTime = time(NULL);

}  // namespace leveldb
struct WikipediaRow {
    int32_t cur_id;
    int32_t cur_namespace;
    std::string cur_title;
    std::string cur_text;
    std::string cur_comment;
    uint64_t cur_user;
    std::string cur_user_text;
    std::string cur_timestamp;
    std::string cur_restrictions;
    int32_t	cur_counter;
    int32_t cur_is_redirect;
    int32_t cur_minor_edit;
    std::string cur_random;
    std::string cur_touched;
    std::string inverse_timestamp;

    DATA_IO_LOAD_SAVE(WikipediaRow,
                      &cur_id
                      &cur_namespace
                      &Schema::StrZero(cur_title)
                      &Schema::StrZero(cur_text)
                      &Schema::StrZero(cur_comment)
                      &cur_user
                      &Schema::StrZero(cur_user_text)
                      &Schema::StrZero(cur_timestamp)
                      &Schema::StrZero(cur_restrictions)
                      &cur_counter
                      &cur_is_redirect
                      &cur_minor_edit
                      &Schema::StrZero(cur_random)
                      &Schema::StrZero(cur_touched)
                      &Schema::StrZero(inverse_timestamp)
    )
};
#endif //TERARKDB_TEST_FRAMEWORK_LEVELDB_H
