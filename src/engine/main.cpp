// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

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

using namespace terark;
using namespace db;

// Comma-separated list of operations to run in the specified order
//   Actual benchmarks:
//      fillseq       -- write N values in sequential key order in async mode
//      fillrandom    -- write N values in random key order in async mode
//      overwrite     -- overwrite N values in random key order in async mode
//      fillsync      -- write N/100 values in random key order in sync mode
//      fill100K      -- write N/1000 100K values in random order in async mode
//      deleteseq     -- delete N keys in sequential order
//      deleterandom  -- delete N keys in random order
//      readseq       -- read N times sequentially
//      readreverse   -- read N times in reverse order
//      readrandom    -- read N times in random order
//      readmissing   -- read N missing keys in random order
//      readhot       -- read N times in random order from 1% section of DB
//      seekrandom    -- N random seeks
//      crc32c        -- repeated crc32c of 4K of data
//      acquireload   -- load N*1000 times
//   Meta operations:
//      compact     -- Compact the entire DB
//      stats       -- Print DB stats
//      sstables    -- Print sstable info
//      heapprofile -- Dump a heap profile (if supported by this port)


// Number of key/values to place in database

//Setting setting;
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
        public:
            Stats(Setting &setting1) :setting(setting1) { Start(); }

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

            ThreadState(int index,Setting &setting1)
                    : tid(index),
                      rand(1000 + index) {
                stats = new Stats(setting1);
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

    class Benchmark {
    private:
        CompositeTablePtr tab;

        fstrvec allkeys_;

        int num_;
        int value_size_;
        int entries_per_batch_;
        WriteOptions write_options_;
        int reads_;
        int heap_counter_;
        Setting &setting;
        void PrintHeader() {
            fprintf(stdout, "NarkDB Test Begins!");
        }

        void PrintWarnings() {
#if defined(__GNUC__) && !defined(__OPTIMIZE__)
            fprintf(stdout,
                    "WARNING: Optimization is disabled: benchmarks unnecessarily slow\n"
            );
#endif
#ifndef NDEBUG
            fprintf(stdout,
                    "WARNING: Assertions are enabled; benchmarks unnecessarily slow\n");
#endif

            // See if snappy is working by attempting to compress a compressible string
            const char text[] = "yyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyy";
            std::string compressed;
            if (!port::Snappy_Compress(text, sizeof(text), &compressed)) {
                fprintf(stdout, "WARNING: Snappy compression is not enabled\n");
            } else if (compressed.size() >= sizeof(text)) {
                fprintf(stdout, "WARNING: Snappy compression is not effective\n");
            }
        }

        void PrintEnvironment() {
            fprintf(stdout, "LevelDB:    version %d.%d\n",
                    kMajorVersion, kMinorVersion);
            fprintf(stderr, "LevelDB:    version %d.%d\n",
                    kMajorVersion, kMinorVersion);

#if defined(__linux)
            time_t now = time(NULL);
            fprintf(stderr, "Date:       %s", ctime(&now));  // ctime() adds newline

            FILE* cpuinfo = fopen("/proc/cpuinfo", "r");
            if (cpuinfo != NULL) {
                char line[1000];
                int num_cpus = 0;
                std::string cpu_type;
                std::string cache_size;
                while (fgets(line, sizeof(line), cpuinfo) != NULL) {
                    const char* sep = strchr(line, ':');
                    if (sep == NULL) {
                        continue;
                    }
                    Slice key = TrimSpace(Slice(line, sep - 1 - line));
                    Slice val = TrimSpace(Slice(sep + 1));
                    if (key == "model name") {
                        ++num_cpus;
                        cpu_type = val.ToString();
                    } else if (key == "cache size") {
                        cache_size = val.ToString();
                    }
                }
                fclose(cpuinfo);
                fprintf(stderr, "CPU:        %d * %s\n", num_cpus, cpu_type.c_str());
                fprintf(stderr, "CPUCache:   %s\n", cache_size.c_str());
            }
#endif
        }

    public:
        Benchmark(Setting &setting1)
                : tab(NULL),
                  num_(setting1.FLAGS_num),
                  value_size_(setting1.FLAGS_value_size),
                  entries_per_batch_(1),
                  reads_(setting1.FLAGS_reads < 0 ? setting1.FLAGS_num : setting1.FLAGS_reads),
                  heap_counter_(0),
                  setting(setting1)
                {
            std::vector<std::string> files;
            Env::Default()->GetChildren(setting.FLAGS_db, &files);
            for (int i = 0; i < files.size(); i++) {
                if (Slice(files[i]).starts_with("heap-")) {
                    Env::Default()->DeleteFile(std::string(setting.FLAGS_db) + "/" + files[i]);
                }
            }
            if (!setting.FLAGS_use_existing_db) {
                DestroyDB(setting.FLAGS_db, Options());
            }
        }

        ~Benchmark() {
            tab->safeStopAndWaitForCompress();
            tab = NULL;
        }

        void Run() {
            PrintHeader();
            std::cout << " Run() " << std::endl;

            std::ifstream ifs(setting.FLAGS_resource_data);
            std::string str;
            std::string key1;
            std::string key2;

            while(getline(ifs, str)) {
                if (str.find("product/productId:") == 0) {
                    key1 = str.substr(19);
                    continue;
                }
                if (str.find("review/userId:") == 0) {
                    key2 = str.substr(15);
                    allkeys_.push_back(key1 + " " + key2);
                    continue;
                }
            }
            allkeys_.shrink_to_fit();
            printf("allkeys_.mem_size=%zd\n", allkeys_.full_mem_size());
            assert(allkeys_.size() == setting.FLAGS_num);

            Open();

            const char* benchmarks = setting.FLAGS_benchmarks;
            while (benchmarks != NULL) {
                const char* sep = strchr(benchmarks, ',');
                Slice name;
                if (sep == NULL) {
                    name = benchmarks;
                    benchmarks = NULL;
                } else {
                    name = Slice(benchmarks, sep - benchmarks);
                    benchmarks = sep + 1;
                }

                printf("benchmark name %s\n", name.data());

                // Reset parameters that may be overriddden bwlow
                num_ = setting.FLAGS_num;
                reads_ = (setting.FLAGS_reads < 0 ? setting.FLAGS_num : setting.FLAGS_reads);
                value_size_ = setting.FLAGS_value_size;
                entries_per_batch_ = 1;
                write_options_ = WriteOptions();

                void (Benchmark::*method)(ThreadState*) = NULL;
                bool fresh_db = false;
                int num_threads = setting.FLAGS_threads;

                if (name == Slice("fillseq")) {
                    fresh_db = true;
                    method = &Benchmark::WriteSeq;
                } else if (name == Slice("fillseqbatch")) {
                    fresh_db = true;
                    entries_per_batch_ = 1000;
                    method = &Benchmark::WriteSeq;
                } else if (name == Slice("fillrandbatch")) {
                    fresh_db = true;
                    entries_per_batch_ = 1000;
                    method = &Benchmark::WriteRandom;
                } else if (name == Slice("fillrandom")) {
                    fresh_db = true;
                    method = &Benchmark::WriteRandom;

                    Random rand(1000);
                    rand.Shuffle(setting.shuff, setting.FLAGS_threads);

                } else if (name == Slice("overwrite")) {
                    fresh_db = false;
                    method = &Benchmark::WriteRandom;
                } else if (name == Slice("fillseqsync")) {
                    fresh_db = true;
#if 1
                    num_ /= 1000;
                    if (num_<10) num_=10;
#endif
                    write_options_.sync = true;
                    method = &Benchmark::WriteSeq;
                } else if (name == Slice("fillrandsync")) {
                    fresh_db = true;
#if 1
                    num_ /= 1000;
                    if (num_<10) num_=10;
#endif
                    write_options_.sync = true;
                    method = &Benchmark::WriteRandom;
                } else if (name == Slice("fill100K")) {
                    fresh_db = true;
                    num_ /= 1000;
                    value_size_ = 100 * 1000;
                    method = &Benchmark::WriteRandom;
                } else if (name == Slice("readseq")) {
                    method = &Benchmark::ReadSequential;
                } else if (name == Slice("readreverse")) {
                    method = &Benchmark::ReadReverse;
                } else if (name == Slice("readrandom")) {
                    method = &Benchmark::ReadRandom;
                } else if (name == Slice("readmissing")) {
                    method = &Benchmark::ReadMissing;
                } else if (name == Slice("seekrandom")) {
                    method = &Benchmark::SeekRandom;
                } else if (name == Slice("readhot")) {
                    method = &Benchmark::ReadHot;
                } else if (name == Slice("readrandomsmall")) {
                    reads_ /= 1000;
                    method = &Benchmark::ReadRandom;
                } else if (name == Slice("deleteseq")) {
                    method = &Benchmark::DeleteSeq;
                } else if (name == Slice("deleterandom")) {
                    method = &Benchmark::DeleteRandom;
                } else if (name == Slice("readwhilewriting")) {
                    // num_threads++;  // Add extra thread for writing
                    // method = &Benchmark::ReadWhileWriting;
                    // method = &Benchmark::ReadWhileWritingNew;
                    method = &Benchmark::ReadWhileWritingNew4;
                    Random rand(1000);
                    rand.Shuffle(setting.shuff, setting.FLAGS_threads);
                } else if (name == Slice("readwritedel")) {
                    method = &Benchmark::ReadWriteDel;
                } else if (name == Slice("compact")) {
                    method = &Benchmark::Compact;
                } else if (name == Slice("crc32c")) {
                    method = &Benchmark::Crc32c;
                } else if (name == Slice("acquireload")) {
                    method = &Benchmark::AcquireLoad;
                } else if (name == Slice("heapprofile")) {
                    HeapProfile();
                } else if (name == Slice("stats")) {
                    PrintStats("leveldb.stats");
                } else if (name == Slice("sstables")) {
                    PrintStats("leveldb.sstables");
                } else {
                    if (name != Slice()) {  // No error message for empty name
                        fprintf(stderr, "unknown benchmark '%s'\n", name.ToString().c_str());
                    }
                }

                if (fresh_db) {
                    if (setting.FLAGS_use_existing_db) {
                        /*do nothing*/
                    } else {
                        tab = NULL;
                        Open();
                    }
                }

                struct timespec start, end;
                clock_gettime(CLOCK_MONOTONIC, &start);
                if (method != NULL) {
                    printf("RunBenchmark %s\n", name.data());
                    RunBenchmark(num_threads, name, method);
                }
                clock_gettime(CLOCK_MONOTONIC, &end);
                long long timeuse = 1000000000LL * ( end.tv_sec - start.tv_sec ) + end.tv_nsec -start.tv_nsec;
                printf("RunBenchmark total time is : %lld \n", timeuse/1000000000LL);
                tab->syncFinishWriting();
            }
            allkeys_.erase_all();
        }

    private:
        struct ThreadArg {
            Benchmark* bm;
            SharedState* shared;
            ThreadState* thread;
            void (Benchmark::*method)(ThreadState*);
        };

        static void ThreadBody(void* v) {
            ThreadArg* arg = reinterpret_cast<ThreadArg*>(v);
            SharedState* shared = arg->shared;
            ThreadState* thread = arg->thread;
            {
                MutexLock l(&shared->mu);
                shared->num_initialized++;
                if (shared->num_initialized >= shared->total) {
                    shared->cv.SignalAll();
                }
                while (!shared->start) {
                    shared->cv.Wait();
                }
            }

            thread->stats->Start();
            (arg->bm->*(arg->method))(thread);
            thread->stats->Stop();

            {
                MutexLock l(&shared->mu);
                shared->num_done++;
                if (shared->num_done >= shared->total) {
                    shared->cv.SignalAll();
                }
            }
        }

        void RunBenchmark(int n, Slice name,
                          void (Benchmark::*method)(ThreadState*)) {
            SharedState shared;
            shared.total = n;
            shared.num_initialized = 0;
            shared.num_done = 0;
            shared.start = false;

            ThreadArg* arg = new ThreadArg[n];
            for (int i = 0; i < n; i++) {
                arg[i].bm = this;
                arg[i].method = method;
                arg[i].shared = &shared;
                arg[i].thread = new ThreadState(i,setting);
                arg[i].thread->shared = &shared;
                Env::Default()->StartThread(ThreadBody, &arg[i]);
            }

            shared.mu.Lock();
            while (shared.num_initialized < n) {
                shared.cv.Wait();
            }

            shared.start = true;
            shared.cv.SignalAll();
            while (shared.num_done < n) {
                shared.cv.Wait();
            }
            shared.mu.Unlock();

            for (int i = 1; i < n; i++) {
                arg[0].thread->stats->Merge(*(arg[i].thread->stats));
            }
            arg[0].thread->stats->Report(name);

            for (int i = 0; i < n; i++) {
                delete arg[i].thread;
            }
            delete[] arg;
        }

        void Crc32c(ThreadState* thread) {
            // Checksum about 500MB of data total
            const int size = 4096;
            const char* label = "(4K per op)";
            std::string data(size, 'x');
            int64_t bytes = 0;
            uint32_t crc = 0;
            while (bytes < 500 * 1048576) {
                crc = crc32c::Value(data.data(), size);
                thread->stats->FinishedSingleOp();
                bytes += size;
            }
            // Print so result is not dead
            fprintf(stderr, "... crc=0x%x\r", static_cast<unsigned int>(crc));

            thread->stats->AddBytes(bytes);
            thread->stats->AddMessage(label);
        }

        void AcquireLoad(ThreadState* thread) {
            int dummy;
            port::AtomicPointer ap(&dummy);
            int count = 0;
            void *ptr = NULL;
            thread->stats->AddMessage("(each op is 1000 loads)");
            while (count < 100000) {
                for (int i = 0; i < 1000; i++) {
                    ptr = ap.Acquire_Load();
                }
                count++;
                thread->stats->FinishedSingleOp();
            }
            if (ptr == NULL) exit(1); // Disable unused variable warning.
        }


        void Open() {
            assert(tab == NULL);
            std::cout << "Create database " << setting.FLAGS_db << std::endl;

            tab = CompositeTable::createTable(setting.FLAGS_db_table);
            tab->load(setting.FLAGS_db);
        }

        void WriteSeq(ThreadState* thread) {
            DoWrite(thread, true);
        }

        void WriteRandom(ThreadState* thread) {
            DoWrite(thread, false);
        }

        void DoWrite(ThreadState* thread, bool seq) {
            std::cout << " DoWrite now! num_ " << num_ << " setting.FLAGS_num " << setting.FLAGS_num << std::endl;

            DbContextPtr ctxw;
            ctxw = tab->createDbContext();
            ctxw->syncIndex = setting.FLAGS_sync_index;

            if (num_ != setting.FLAGS_num) {
                char msg[100];
                snprintf(msg, sizeof(msg), "(%d ops)", num_);
                thread->stats->AddMessage(msg);
            }


            terark::NativeDataOutput<terark::AutoGrownMemIO> rowBuilder;
            std::ifstream ifs(setting.FLAGS_resource_data);
            std::string str;

            int64_t avg = setting.FLAGS_num/setting.FLAGS_threads;
            int64_t copyavg = avg;
            int offset = setting.shuff[thread->tid];
            if (avg != setting.FLAGS_num) {
                if (offset != 0) {
                    int64_t skip = offset * avg;
                    if (skip != 0) {
                        while(getline(ifs, str)) {
                            if (str.find("review/text:") == 0) {
                                skip --;
                                if (skip == 0)
                                    break;
                            }
                        }
                    }
                }
            }

            std::string key1;
            std::string key2;
            TestRow recRow;
            int64_t writen = 0;

            while(getline(ifs, str) && avg != 0) {
                fstring fstr(str);
                if (fstr.startsWith("product/productId:")) {
                    key1 = str.substr(19);
                    continue;
                }
                if (fstr.startsWith("review/userId:")) {
                    key2 = str.substr(15);
                    continue;
                }
                if (fstr.startsWith("review/profileName:")) {
                    recRow.profileName = str.substr(20);
                    continue;
                }
                if (fstr.startsWith("review/helpfulness:")) {
                    char* pos2 = NULL;
                    recRow.helpfulness1 = strtol(fstr.data()+20, &pos2, 10);
                    recRow.helpfulness2 = strtol(pos2+1, NULL, 10);
                    continue;
                }
                if (fstr.startsWith("review/score:")) {
                    recRow.score = lcast(fstr.substr(14));
                    continue;
                }
                if (fstr.startsWith("review/time:")) {
                    recRow.time = lcast(fstr.substr(13));
                    continue;
                }
                if (fstr.startsWith("review/summary:")) {
                    recRow.summary = str.substr(16);
                    continue;
                }
                if (fstr.startsWith("review/text:")) {
                    recRow.text = str.substr(13);
                    recRow.product_userId = key1 + " " + key2;

                    rowBuilder.rewind();
                    rowBuilder << recRow;
                    fstring binRow(rowBuilder.begin(), rowBuilder.tell());

                    // if (ctxw->insertRow(binRow) < 0) { // non unique index
                    if (ctxw->upsertRow(binRow) < 0) { // unique index
                        printf("Insert failed: %s\n", ctxw->errMsg.c_str());
                        exit(-1);
                    }
                    thread->stats->FinishedSingleOp();
                    writen ++;
                    avg --;
                    continue;
                }
            }
            time_t now;
            struct tm *timenow;
            time(&now);
            timenow = localtime(&now);
            printf("writenum %lld, avg %lld, offset %d, time %s\n",writen, copyavg, offset, asctime(timenow));
        }

        void ReadSequential(ThreadState* thread) {
            fprintf(stderr, "ReadSequential not supported\n");
            return;
        }

        void ReadReverse(ThreadState* thread) {
            fprintf(stderr, "ReadReverse not supported\n");
            return;
        }

        void ReadRandom(ThreadState* thread) {
            valvec<byte> keyHit, val;
            valvec<valvec<byte> > cgDataVec;
            valvec<llong> idvec;
            valvec<size_t> colgroups;
            DbContextPtr ctxr;
            ctxr = tab->createDbContext();
            ctxr->syncIndex = setting.FLAGS_sync_index;
            // std::cout << " tab->getIndexNum() " << tab->getIndexNum() << " tab->getColgroupNum() " << tab->getColgroupNum() << std::endl;
            for (size_t i = tab->getIndexNum(); i < tab->getColgroupNum(); i++) {
                colgroups.push_back(i);
            }

            int *shuffr = NULL;
            shuffr = (int *)malloc(setting.FLAGS_num * sizeof(int));
            printf("Thread:%d ReadRandom. setting.FLAGS_num:%d allkeys_.size():%d reads_:%d\n",
                   thread->tid,setting.FLAGS_num,allkeys_.size(),reads_);
            for (int i=0; i<setting.FLAGS_num; i++)
                shuffr[i] = i % allkeys_.size();
            thread->rand.Shuffle(shuffr, setting.FLAGS_num);

            int found = 0;
            size_t indexId = 0;

            for (size_t i = 0; i < reads_; ++i) {
                int k = shuffr[i%setting.FLAGS_num];
                //printf("Thread:%d ReadRandom. allkeys_.size():%d  k:%d\n",thread->tid,allkeys_.size(),k);

                fstring key(allkeys_.at(k));

                tab->indexSearchExact(indexId, key, &idvec, ctxr.get());
                for (auto recId : idvec) {
                    tab->selectColgroups(recId, colgroups, &cgDataVec, ctxr.get());
                }
                if(idvec.size() > 0)
                    found++;
                thread->stats->FinishedSingleOp();
            }
            char msg[100];
            snprintf(msg, sizeof(msg), "(%d of %d found)", found, num_);
            thread->stats->AddMessage(msg);

        }

        void ReadMissing(ThreadState* thread) {
            fprintf(stderr, "ReadMissing not supported\n");
            return;
        }

        void ReadHot(ThreadState* thread) {
            valvec<byte> val;
            llong recId;

            DbContextPtr ctxr;
            ctxr = tab->createDbContext();
            ctxr->syncIndex = setting.FLAGS_sync_index;
            const int range = (setting.FLAGS_num + 99) / 100;
            for (int i = 0; i < reads_; i++) {
                recId = thread->rand.Next() % range;
                ctxr->getValue(recId, &val);
                thread->stats->FinishedSingleOp();
            }
        }

        void SeekRandom(ThreadState* thread) {
            fprintf(stderr, "SeekRandom not supported\n");
            return;
        }

        void DoDelete(ThreadState* thread, bool seq) {
            fprintf(stderr, "DoDelete not supported\n");
            return;
        }

        void DeleteSeq(ThreadState* thread) {
            DoDelete(thread, true);
        }

        void DeleteRandom(ThreadState* thread) {
            DoDelete(thread, false);
        }

        void ReadWriteDel(ThreadState* thread) {
            if (thread->tid % 3 == 0) { // read
                ReadRandom(thread);
            } else if (thread->tid % 3 == 1) { // write
                int64_t num = 0;
                terark::NativeDataOutput<terark::AutoGrownMemIO> rowBuilder;
                DbContextPtr ctxw;
                ctxw = tab->createDbContext();
                ctxw->syncIndex = setting.FLAGS_sync_index;

                while(true) {
                    std::ifstream ifs(setting.FLAGS_resource_data);
                    std::string str;
                    std::string key1;
                    std::string key2;

                    TestRow recRow;

                    while(getline(ifs, str)) {
                        fstring fstr(str);
                        if (fstr.startsWith("product/productId:")) {
                            key1 = str.substr(19);
                        }
                        if (fstr.startsWith("review/userId:")) {
                            key2 = str.substr(15);
                        }
                        if (fstr.startsWith("review/profileName:")) {
                            recRow.profileName = str.substr(20);
                        }
                        if (fstr.startsWith("review/helpfulness:")) {
                            char* pos2 = NULL;
                            recRow.helpfulness1 = strtol(fstr.data()+20, &pos2, 10);
                            recRow.helpfulness2 = strtol(pos2+1, NULL, 10);
                        }
                        if (fstr.startsWith("review/score:")) {
                            recRow.score = lcast(fstr.substr(14));
                        }
                        if (fstr.startsWith("review/time:")) {
                            recRow.time = lcast(fstr.substr(13));
                        }
                        if (fstr.startsWith("review/summary:")) {
                            recRow.summary = str.substr(16);
                        }
                        if (fstr.startsWith("review/text:")) {
                            recRow.text = str.substr(13);
                            recRow.product_userId = key1 + " " + key2;

                            rowBuilder.rewind();
                            rowBuilder << recRow;
                            fstring binRow(rowBuilder.begin(), rowBuilder.tell());

                            if (ctxw->insertRow(binRow) < 0) {
                                printf("Insert failed: %s\n", ctxw->errMsg.c_str());
                                exit(-1);
                            }
                            num ++;
                        }

                        MutexLock l(&thread->shared->mu);
                        if (thread->shared->num_done + 2*setting.FLAGS_threads/3 >= thread->shared->num_initialized) {
                            printf("extra write operations number %d\n", num);
                            return;
                        }
                    }
                }
            } else {  // del
                valvec<byte> keyHit, val;
                valvec<llong> idvec;
                DbContextPtr ctxr;
                ctxr = tab->createDbContext();
                ctxr->syncIndex = setting.FLAGS_sync_index;
                int64_t num = 0;

                size_t indexId = 0;
                while(true) {
                    const int k = thread->rand.Next() % setting.FLAGS_num;
                    fstring key(allkeys_.at(k));

                    tab->indexSearchExact(indexId, key, &idvec, ctxr.get());
                    for (auto recId : idvec) {
                        ctxr->removeRow(recId);
                    }
                    num ++;
                    MutexLock l(&thread->shared->mu);
                    if (thread->shared->num_done + 2*setting.FLAGS_threads/3 >= thread->shared->num_initialized) {
                        printf("extra del operations number %d\n", num);
                        return;
                    }
                }
            }
        }



        void ReadWhileWritingNew4(ThreadState* thread) {
            int64_t readold = 0;
            int64_t readnew = 0;
            int64_t writenadd = 0;
            int64_t writenupdate = 0;
            valvec<valvec<byte> > cgDataVec;
            valvec<llong> idvec;
            valvec<size_t> colgroups;
            DbContextPtr ctxrw = tab->createDbContext();
            ctxrw->syncIndex = setting.FLAGS_sync_index;
            for (size_t i = tab->getIndexNum(); i < tab->getColgroupNum(); i++) {
                colgroups.push_back(i);
            }
            int found = 0;
            size_t indexId = 0;
            terark::NativeDataOutput<terark::AutoGrownMemIO> rowBuilder;
            terark::AutoFree<char> szPath;
            asprintf(&szPath.p, "%s.%d", setting.FLAGS_resource_data, thread->tid);
            std::ifstream ifs(szPath.p);
            std::string str;
            std::string key1;
            std::string key2;

            TestRow recRow;
            struct timespec start, end;
            long long readtime = 0;
            long long writetime = 0;
            long long totaltime = 0;
            double percent = setting.FLAGS_read_write_percent / 100.0;
            double percenr = setting.FLAGS_read_old_record_percent / 100.0;
            double percentw = setting.FLAGS_write_new_record_percent / 100.0;
            long long newrecord = 1;
            std::string record;
            std::stringstream recordstreamo;
            recordstreamo << newrecord;
            recordstreamo >> record;

            clock_gettime(CLOCK_MONOTONIC, &start);
            int i = 0;
            while(1) {
                size_t rdi = thread->rand.Next();
                double rdd = rdi / double(INT32_MAX);
                if (rdd < percent) {
                    size_t k = thread->rand.Next() % allkeys_.size();
                    size_t rdir = thread->rand.Next();
                    double rddr = rdir / double(INT32_MAX);
                    if (rddr < percenr) {
                        fstring key(allkeys_.at(k));
                        ctxrw->indexSearchExact(indexId, key, &idvec);
                        for (auto recId : idvec) {
                            ctxrw->selectColgroups(recId, colgroups, &cgDataVec);
                        }
                        if(idvec.size() > 0)
                            found++;
                        readold++;
                    } else {
                        std::string merge = allkeys_.at(k) + " " + record;
                        fstring key(merge);
                        ctxrw->indexSearchExact(indexId, key, &idvec);
                        for (auto recId : idvec) {
                            ctxrw->selectColgroups(recId, colgroups, &cgDataVec);
                        }
                        if(idvec.size() > 0)
                            found++;
                        readnew++;
                    }
                    thread->stats->FinishedSingleOp();
                } else {
                    size_t rdiw = thread->rand.Next();
                    double rddw = rdiw / double(INT32_MAX);
                    if (rddw < percentw) {   // add new record
                        while(getline(ifs, str)) {
                            fstring fstr(str);
                            if (fstr.startsWith("product/productId:")) {
                                key1 = str.substr(19);
                                continue;
                            }
                            if (fstr.startsWith("review/userId:")) {
                                key2 = str.substr(15);
                                continue;
                            }
                            if (fstr.startsWith("review/profileName:")) {
                                recRow.profileName = str.substr(20);
                                continue;
                            }
                            if (fstr.startsWith("review/helpfulness:")) {
                                char* pos2 = NULL;
                                recRow.helpfulness1 = strtol(fstr.data()+20, &pos2, 10);
                                recRow.helpfulness2 = strtol(pos2+1, NULL, 10);
                                continue;
                            }
                            if (fstr.startsWith("review/score:")) {
                                recRow.score = lcast(fstr.substr(14));
                                continue;
                            }
                            if (fstr.startsWith("review/time:")) {
                                recRow.time = lcast(fstr.substr(13));
                                continue;
                            }
                            if (fstr.startsWith("review/summary:")) {
                                recRow.summary = str.substr(16);
                                continue;
                            }
                            if (fstr.startsWith("review/text:")) {
                                recRow.text = str.substr(13);
                                recRow.product_userId = key1 + " " + key2 + " " + record;
                                rowBuilder.rewind();
                                rowBuilder << recRow;
                                fstring binRow(rowBuilder.begin(), rowBuilder.tell());

                                if (ctxrw->upsertRow(binRow) < 0) { // unique index
                                    printf("Insert failed: %s\n", ctxrw->errMsg.c_str());
                                    continue;
                                }
                                writenadd++;
                                thread->stats->FinishedSingleOp();
                                break;
                            }
                        }
                    } else {   // update origin record
                        while(getline(ifs, str)) {
                            fstring fstr(str);
                            if (fstr.startsWith("product/productId:")) {
                                key1 = str.substr(19);
                                continue;
                            }
                            if (fstr.startsWith("review/userId:")) {
                                key2 = str.substr(15);
                                continue;
                            }
                            if (fstr.startsWith("review/profileName:")) {
                                recRow.profileName = str.substr(20);
                                continue;
                            }
                            if (fstr.startsWith("review/helpfulness:")) {
                                char* pos2 = NULL;
                                recRow.helpfulness1 = strtol(fstr.data()+20, &pos2, 10);
                                recRow.helpfulness2 = strtol(pos2+1, NULL, 10);
                                continue;
                            }
                            if (fstr.startsWith("review/score:")) {
                                recRow.score = lcast(fstr.substr(14));
                                continue;
                            }
                            if (fstr.startsWith("review/time:")) {
                                recRow.time = lcast(fstr.substr(13));
                                continue;
                            }
                            if (fstr.startsWith("review/summary:")) {
                                recRow.summary = str.substr(16);
                                continue;
                            }
                            if (fstr.startsWith("review/text:")) {
                                recRow.text = str.substr(13);
                                recRow.product_userId = key1 + " " + key2;

                                rowBuilder.rewind();
                                rowBuilder << recRow;
                                fstring binRow(rowBuilder.begin(), rowBuilder.tell());

                                if (ctxrw->upsertRow(binRow) < 0) { // unique index
                                    printf("Insert failed: %s\n", ctxrw->errMsg.c_str());
                                    continue;
                                }
                                writenupdate++;
                                thread->stats->FinishedSingleOp();
                                break;
                            }
                        }
                    }
                    if(ifs.eof()) {
                        ifs.clear();
                        ifs.seekg(0, std::ios::beg);
                        newrecord ++;
                        std::stringstream recordstream;
                        recordstream << newrecord;
                        recordstream >> record;
                    }
                }
                i++;
                if((i+1)%1000 == 0) {
                    clock_gettime(CLOCK_MONOTONIC, &end);
                    long long timeuse = 1000000000LL * ( end.tv_sec - start.tv_sec ) + end.tv_nsec -start.tv_nsec;
                    totaltime += timeuse;
                    //printf("i %d thread %d current qps %0.2f, total qps %0.2f, timeuse %f totaltime %f readold %lld readnew %lld writeadd %lld writeupdate %lld\n", i, thread->tid, 1000.0/(timeuse/1000000000.0), i/(totaltime/1000000000.0), timeuse/1000000000.0, totaltime/1000000000.0, readold, readnew, writenadd, writenupdate);
                    clock_gettime(CLOCK_MONOTONIC, &start);
                }

            }
            time_t now;
            struct tm *timenow;
            time(&now);
            timenow = localtime(&now);
        }




       

        void Compact(ThreadState* thread) {
            fprintf(stderr, "Compact not supported\n");
            return;
        }

        void PrintStats(const char* key) {
            fprintf(stderr, "PrintStats not supported\n");
            return;
        }

        static void WriteToFile(void* arg, const char* buf, int n) {
            reinterpret_cast<WritableFile*>(arg)->Append(Slice(buf, n));
        }

        void HeapProfile() {
            char fname[100];
            snprintf(fname, sizeof(fname), "%s/heap-%04d", setting.FLAGS_db, ++heap_counter_);
            WritableFile* file;
            Status s = Env::Default()->NewWritableFile(fname, &file);
            if (!s.ok()) {
                fprintf(stderr, "%s\n", s.ToString().c_str());
                return;
            }
            bool ok = port::GetHeapProfile(WriteToFile, file);
            delete file;
            if (!ok) {
                fprintf(stderr, "heap profiling not supported\n");
                Env::Default()->DeleteFile(fname);
            }
        }
    };

}  // namespace leveldb

int main(int argc, char** argv) {
    Setting setting;
    setting.FLAGS_write_buffer_size = leveldb::Options().write_buffer_size;
    setting.FLAGS_open_files = leveldb::Options().max_open_files;
    std::string default_db_path;
    std::string default_db_table;

    for (int i = 1; i < argc; i++) {
        double d;
        int n;
        char junk;
        if (leveldb::Slice(argv[i]).starts_with("--benchmarks=")) {
            setting.FLAGS_benchmarks = argv[i] + strlen("--benchmarks=");
        } else if (sscanf(argv[i], "--compression_ratio=%lf%c", &d, &junk) == 1) {
            setting.FLAGS_compression_ratio = d;
        } else if (sscanf(argv[i], "--histogram=%d%c", &n, &junk) == 1 &&
                   (n == 0 || n == 1)) {
            setting.FLAGS_histogram = n;
        } else if (sscanf(argv[i], "--use_existing_db=%d%c", &n, &junk) == 1 &&
                   (n == 0 || n == 1)) {
            setting.FLAGS_use_existing_db = n;
        } else if (sscanf(argv[i], "--sync_index=%d%c", &n, &junk) == 1 &&
                   (n == 0 || n == 1)) {
            setting.FLAGS_sync_index = n;
        } else if (sscanf(argv[i], "--num=%d%c", &n, &junk) == 1) {
            setting.FLAGS_num = n;
        } else if (sscanf(argv[i], "--reads=%d%c", &n, &junk) == 1) {
            setting.FLAGS_reads = n;
        } else if (sscanf(argv[i], "--threads=%d%c", &n, &junk) == 1) {
            setting.FLAGS_threads = n;
        } else if (strncmp(argv[i], "--db=", 5) == 0) {
            setting.FLAGS_db = argv[i] + 5;
        } else if (sscanf(argv[i], "--read_ratio=%lf%c", &d, &junk) == 1) {
            setting.FLAGS_read_write_percent = d;
        } else if (sscanf(argv[i], "--read_old_ratio=%lf%c", &d, &junk) == 1) {
            setting.FLAGS_read_old_record_percent = d;
        } else if (sscanf(argv[i], "--write_ratio=%lf%c", &d, &junk) == 1) {
            setting.FLAGS_write_new_record_percent = d;
        } else if (strncmp(argv[i], "--resource_data=", 16) == 0) {
            setting.FLAGS_resource_data = argv[i] + 16;
        } else {
            fprintf(stderr, "Invalid flag '%s'\n", argv[i]);
            exit(1);
        }
    }

    // Choose a location for the test database if none given with --db=<path>
    if (setting.FLAGS_db == NULL) {
        leveldb::Env::Default()->GetTestDirectory(&default_db_path);
        default_db_path += "/dbbench";
        setting.FLAGS_db = default_db_path.c_str();
    }

    if (setting.FLAGS_db_table == NULL) {
        default_db_table += "DfaDbTable";
        setting.FLAGS_db_table = default_db_table.c_str();
    }

    if (setting.FLAGS_resource_data == NULL) {
        fprintf(stderr, "Please input the resource data file\n");
        exit(-1);
    }

    setting.shuff = (int *)malloc(setting.FLAGS_threads * sizeof(int));
    for (int i=0; i<setting.FLAGS_threads; i++)
        setting.shuff[i] = i;

    leveldb::Benchmark benchmark(setting);
    benchmark.Run();
    fprintf(stdout, "db movies terark completed\n");
    return 0;
}
