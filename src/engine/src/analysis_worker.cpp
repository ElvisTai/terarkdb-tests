//
// Created by terark on 16-8-11.
//
#include "stdlib.h"
#include "inttypes.h"
#include "analysis_worker.h"
#include "util/system_resource.h"
#include <terark/io/MemStream.hpp>
#include <terark/lcast.hpp>
#include <boost/scope_exit.hpp>
#include "cppconn/prepared_statement.h"
#include "mysql_driver.h"
#include <mysql.h>

using terark::lcast;
using terark::fstring;

class TimeBucket {
private:
    const char* engine_name;
    const std::vector<std::string>& dbdirs;

    int current_bucket = 0;  // seconds
    int operation_count = 0;

public:
    TimeBucket(MYSQL *const connection,
               const char* engineName,
               const std::vector<std::string>& dbdirs)
            : engine_name(engineName),
              dbdirs(dbdirs)
    {}

    void add(terark::AutoGrownMemIO& buf, uint64_t start, uint64_t end, int sampleRate, int type);
};

static int findTimeBucket(uint64_t time) {
    static const uint64_t step_in_seconds = 10; // in seconds
    uint64_t t = time / (1000 * 1000 * 1000 * step_in_seconds);
    // printf("find time bucket : %" PRIu64 ", result = %" PRIu64 "\n", time, t*10);
    return t*10;
}

void Bind_arg(MYSQL_BIND &b, const int &val) {
    memset(&b, 0, sizeof(b));
    b.buffer_length = 4;
    b.buffer_type = MYSQL_TYPE_LONG;
    b.buffer = (void*)&val;
}
void Bind_arg(MYSQL_BIND& b, const double& val) {
    memset(&b, 0, sizeof(b));
    b.buffer_length = 4;
    b.buffer_type = MYSQL_TYPE_DOUBLE;
    b.buffer = (void*)&val;
}
void Bind_arg(MYSQL_BIND &b, fstring val) {
    memset(&b, 0, sizeof(b));
    b.buffer_length = val.size();
    b.buffer_type = MYSQL_TYPE_VAR_STRING;
    b.buffer = (void*)val.data();
}
template<class... Args>
bool Exec_stmt(MYSQL_STMT* stmt, const Args&... args) {
    MYSQL_BIND  b[sizeof...(Args)];
    memset(&b, 0, sizeof(b));
    int i = 0;
    std::initializer_list<int>{(Bind_arg(b[i], args), i++)...};
    mysql_stmt_bind_param(stmt, b);
    int err = mysql_stmt_execute(stmt);
    if (err) {
        fprintf(stderr, "WARN: %s = %s\n", BOOST_CURRENT_FUNCTION, stmt->last_error);
        return false;
    }
    return true;
}

static MYSQL_STMT* prepare(MYSQL* conn, fstring sql) {
    MYSQL_STMT* stmt = mysql_stmt_init(conn);
    int err = mysql_stmt_prepare(stmt, sql.data(), sql.size());
    if (err) {
        fprintf(stderr, "ERROR: mysql_stmt_prepare(%s) = %s\n", sql.c_str(), mysql_error(conn));
        return NULL;
    }
    return stmt;
}
static MYSQL_STMT *ps_ops, *ps_memory, *ps_cpu, *ps_dbsize, *ps_diskinfo;

static int g_prev_sys_stat_bucket = 0;
static void upload_sys_stat(terark::AutoGrownMemIO& buf,
                            const std::vector<std::string>& dbdirs,
                            int bucket, const char* engine_name) {
// 顺便把CPU等数据也上传, 相同时间片只需要上传一次即可
    if (bucket == g_prev_sys_stat_bucket) {
        return;
    }
    g_prev_sys_stat_bucket = bucket;
    int arr[4];
    benchmark::getPhysicalMemoryUsage(arr);
    Exec_stmt(ps_memory, bucket, arr[0], arr[1], arr[2], arr[3], engine_name);
    buf.printf("    total memory = %5.2f GiB", arr[0]/1024.0);

    double cpu[2];
    benchmark::getCPUPercentage(cpu);
    if(cpu > 0){
        Exec_stmt(ps_cpu, bucket, cpu[0], cpu[1], engine_name);
    }
    buf.printf("    cpu usage = %5.2f iowait = %5.2f", cpu[0], cpu[1]);

    int dbsizeKB = benchmark::getDiskUsageByKB(dbdirs);
    if(dbsizeKB > 0) {
        Exec_stmt(ps_dbsize, bucket, dbsizeKB, engine_name);
    }
    buf.printf("    dbsize = %5.2f GiB", dbsizeKB/1024.0/1024);

    std::string diskinfo;
    benchmark::getDiskFileInfo(dbdirs, diskinfo);
    if(diskinfo.length() > 0) {
        Exec_stmt(ps_diskinfo, bucket, fstring(diskinfo), engine_name);
    }
}

void TimeBucket::add(terark::AutoGrownMemIO& buf,uint64_t start, uint64_t end, int sampleRate, int type) {
    // when meet the next bucket, upload previous one first, default step is 10 seconds
    int next_bucket = findTimeBucket(start);
    if(next_bucket > current_bucket) {
        int ops = operation_count * 100 / (10 * sampleRate); // sample rate : (0, 100]
        buf.rewind();
        Exec_stmt(ps_ops, current_bucket, ops, type, engine_name);
        buf.printf("upload statistic time bucket[%d], ops = %d, type = %d", current_bucket, ops, type);
        upload_sys_stat(buf, dbdirs, current_bucket, engine_name);
        printf("%s\n", buf.begin());
        operation_count = 1;
        current_bucket = next_bucket;
    }else{
        operation_count++;
    }
}

AnalysisWorker::AnalysisWorker(Setting* setting) {
    this->setting = setting;
}

AnalysisWorker::~AnalysisWorker() {
    printf("analysis worker is stopped!\n");
}

void AnalysisWorker::stop() {
    shoud_stop = true;
}

void AnalysisWorker::run() {
    char* passwd = getenv("MYSQL_PASSWD");
    std::cout << passwd << std::endl;
    if(passwd == NULL || strlen(passwd) == 0) {
        printf("no MYSQL_PASSWD set, analysis thread will not upload data!\n");
        shoud_stop = true;
        return;
    }
    const char* host = "rds432w5u5d17qd62iq3o.mysql.rds.aliyuncs.com";
    const char* user = "terark_benchmark";
    const char* db = "benchmark";
    int port = 3306;
    my_bool myTrue = true;

    MYSQL conn;
    mysql_init(&conn);
    mysql_options(&conn, MYSQL_OPT_RECONNECT, &myTrue);
    unsigned long clientflag = CLIENT_REMEMBER_OPTIONS;
    if (!mysql_real_connect(&conn, host, user, passwd, db, port, NULL, clientflag)) {
        fprintf(stderr
                , "ERROR: mysql_real_connect(host=%s, user=%s, passwd=%s, db=%s, port=%d, NULL, CLIENT_REMEMBER_OPTIONS) = %s\n"
                , "       database connection fault, monitor data will not be uploaded\n"
                , host, user, passwd, db, port, mysql_error(&conn)
        );
        shoud_stop = true;
        return;
    }
    printf("database connected!\n");
    fflush(stdout);
    // Delete data from 7 days ago.
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    int filter_time = t.tv_sec - 60*60*24*7;
    std::string tables[] = {"engine_test_ops_10s",
                            "engine_test_memory_10s",
                            "engine_test_cpu_10s",
                            "engine_test_dbsize_10s",
                            "engine_test_diskinfo_10s"
    };
    for(std::string& table: tables) {
        std::string sql = "DELETE FROM " + table + " WHERE time_bucket < " + lcast(filter_time);
        mysql_real_query(&conn, sql.c_str(), sql.size());
    }

    ps_ops = prepare(&conn, "INSERT INTO engine_test_ops_10s(time_bucket, ops, ops_type, engine_name) VALUES(?, ?, ?, ?)");
    ps_memory = prepare(&conn, "INSERT INTO engine_test_memory_10s(time_bucket, total_memory, free_memory, cached_memory, used_memory, engine_name) VALUES(?, ?, ?, ?, ?, ?)");
    ps_cpu = prepare(&conn, "INSERT INTO engine_test_cpu_10s(time_bucket, `usage`, `iowait`, engine_name) VALUES(?, ?, ?, ?)");
    ps_dbsize = prepare(&conn, "INSERT INTO engine_test_dbsize_10s(time_bucket, `dbsize`, `engine_name`) VALUES(?, ?, ?)");
    ps_diskinfo = prepare(&conn, "INSERT INTO engine_test_diskinfo_10s(time_bucket, `diskinfo`, `engine_name`) VALUES(?, ?, ?)");

    std::pair<uint64_t, uint64_t> read_result, insert_result, update_result;
    TimeBucket read_bucket(&conn, engine_name.c_str(), setting->dbdirs);
    TimeBucket insert_bucket(&conn, engine_name.c_str(), setting->dbdirs);
    TimeBucket update_bucket(&conn, engine_name.c_str(), setting->dbdirs);

    int prev_bucket = 0;
    terark::AutoGrownMemIO buf;
    shoud_stop = false;
    while(!shoud_stop) {
        bool b1 = Stats::readTimeDataCq.try_pop(read_result);
        bool b2 = Stats::createTimeDataCq.try_pop(insert_result);
        bool b3 = Stats::updateTimeDataCq.try_pop(update_result);
        if(b1){
            read_bucket.add(buf, read_result.first, read_result.second, setting->getSamplingRate(), 1);
        }
        if(b2){
            insert_bucket.add(buf, insert_result.first, insert_result.second, setting->getSamplingRate(), 2);
        }
        if(b3){
            update_bucket.add(buf, update_result.first, update_result.second, setting->getSamplingRate(), 3);
        }
        if(!b1 && !b2 && !b3){
            timespec ts1;
            clock_gettime(CLOCK_REALTIME, &ts1);
            unsigned long long tt = 1000000000ull * ts1.tv_sec + ts1.tv_nsec;
            int curr_bucket = findTimeBucket(tt);
            if (curr_bucket > g_prev_sys_stat_bucket) {
                buf.rewind();
                buf.printf("upload statistic time bucket[%d] no-op", curr_bucket);
                upload_sys_stat(buf, setting->dbdirs, curr_bucket, engine_name.c_str());
                printf("%s\n", buf.begin());
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5000));
        }
    }
    mysql_stmt_close(ps_ops);
    mysql_stmt_close(ps_memory);
    mysql_stmt_close(ps_cpu);
    mysql_stmt_close(ps_dbsize);
    mysql_stmt_close(ps_diskinfo);
    mysql_close(&conn);
}
