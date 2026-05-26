#ifndef FILTERS_SMART_TESTS_HPP
#define FILTERS_SMART_TESTS_HPP

#define PREVENT_PIPELINING (1)
#include "../Tests/wrappers.hpp"
// #include "PerfEvent.hpp"
#include <chrono>
#include <fstream>
#include <random>
#include <unistd.h>
#include <vector>
#include <sys/mman.h>  // For mmap(), munmap(), madvise(), msync()
#include <fcntl.h>     // For open(), O_RDWR, O_CREAT
#include <unistd.h>    // For close(), ftruncate()
#include <sys/stat.h>  // For fstat()

// #include "timing.hpp"
// #include <execution>

typedef std::chrono::nanoseconds ns;

namespace testSmart {
    constexpr size_t db_speeder = 1;
    constexpr size_t ROUNDS = 9;


    #define FIFO_PREFIX "perf_fd"
    #define PERF_CTL_FILE FIFO_PREFIX ".ctl"
    #define PERF_ACK_FILE FIFO_PREFIX ".ack"
    // Class for managing perf monitoring
    class PerfManager {
    
        // control and ack fifo from perf
        int ctl_fd = -1;
        int ack_fd = -1;
        int pid;
        int cpid;

        // if perf is enabled
        bool enable = false;
        bool started = false;
    
        // commands and acks to/from perf
        static constexpr const char* enable_cmd = "enable";
        static constexpr const char* disable_cmd = "disable";
        static constexpr const char* ack_cmd = "ack\n";
    
        // send command to perf via fifo and confirm ack
        void send_command(const char* command) {
            if (enable) {
                if (write(ctl_fd, command, strlen(command)) == -1) {
                    perror("write failed");
                    exit(1);
                }
                
                char ack[5] = {0};
                if (read(ack_fd, ack, 5) == -1) {
                    perror("read failed");
                    exit(1);
                }
                
                if (strcmp(ack, ack_cmd) != 0) {
                    std::cout << "failed acking: " << ack << std::endl;
                    exit(1);
                }
            }
        }
    
      public:
    
        PerfManager() {
            // setup fifo file descriptors
            char* ctl_fd_env = std::getenv("PERF_CTL_FD");
            char* ack_fd_env = std::getenv("PERF_ACK_FD");
            if (ctl_fd_env && ack_fd_env) {
                enable = true;
                ctl_fd = std::stoi(ctl_fd_env);
                ack_fd = std::stoi(ack_fd_env);
            } else {
                std::cout << "FIFO setup failed. ctl fd: " << ctl_fd_env << ", ack fd: " << ctl_fd_env << std::endl;

            }
            std::cout << "FIFO setup complete. ctl fd: " << ctl_fd << ", ack fd: " << ack_fd << std::endl;
        }
    
        // public apis
    
        void pause() {
            if (started) {
                send_command(disable_cmd);
            }
        }
    
        void resume() {
            if (started) {
                send_command(enable_cmd);
            }
        }

        void start() {
            if (!started) {
                pid = getpid();
                cpid = fork();
                if(cpid == 0) {
                    // child process .  Run your perf stat
                    char buf[1024];
                    // sprintf(buf, "/opt/intel/oneapi/vtune/latest/bin64/vtune -collect system-overview -target-pid %d", pid);
                    sprintf(buf, "perf stat -p %d --control fd:%d,%d", pid, ctl_fd, ack_fd); //,
                    printf("running command: %s\n", buf);
                    execl("/bin/sh", "sh", "-c", buf, NULL);
                } else {
                    setpgid(cpid, 0);
                    sleep(1);
                    return;
                }           
            }
            started = true;
        }

        void stop() {
            if (started) {
                kill(cpid, 9);
            }
            started = false;
        }
    };

    size_t count_uniques(std::vector<u64> *v);
    void vector_concatenate_and_shuffle(const std::vector<u64> *v1, const std::vector<u64> *v2, std::vector<u64> *res, std::mt19937_64 rng);

    void vector_concatenate_and_shuffle(const std::vector<u64> *v1, const std::vector<u64> *v2, std::vector<u64> *res);

    void weighted_vector_concatenate_and_shuffle(const std::vector<u64> *v_yes, const std::vector<u64> *v_uni, std::vector<u64> *res, double yes_div, std::mt19937_64 rng);

    size_t test_shuffle_function(const std::vector<u64> *v1, const std::vector<u64> *v2, std::vector<u64> *res);

    void print_data(const u64 *data, size_t size, size_t bench_precision, size_t find_step);


    //Building elements vector

    std::mt19937_64 fill_vec_smart(std::vector<u64> *vec, size_t number_of_elements);

    void my_naive_sample(size_t start, size_t end, size_t length, const std::vector<u64> *input_vec, std::vector<u64> *temp_vec, std::mt19937_64 &rng);

    void fill_vec_by_samples(size_t start, size_t end, size_t length, const std::vector<u64> *input_vec, std::vector<u64> *temp_vec, std::mt19937_64 rng);

    void fill_vec_by_samples(size_t start, size_t end, size_t length, const std::vector<u64> *input_vec, std::vector<u64> *temp_vec);
}// namespace testSmart

namespace testSmart {
    template<typename Table>
    void write_res_to_file_core(const Table *wrap_filter, size_t init_time, size_t filter_max_capacity, size_t lookup_reps, size_t bench_precision, const u64 *data, std::string file_prefix);

    template<typename Table>
    void write_perf_res_to_file(const Table *wrap_filter, size_t init_time, size_t filter_max_capacity, size_t lookup_reps, size_t bench_precision, const u64 *data, std::string file_prefix, std::stringstream &add_ss, std::stringstream &uni_find_ss, std::stringstream &yes_find_ss);

    template<class Table>
    void FPR_test0_after_build(const Table *wrap_filter, const std::vector<u64> *v_add, const std::vector<u64> *v_find, size_t bench_precision);

    template<class Table>
    std::string FPR_parse_data_str_22(const Table *wrap_filter, const std::vector<u64> *v_add, const std::vector<u64> *v_find, size_t yes_res);

    inline size_t sysrandom(void *dst, size_t dstlen) {
        char *buffer = reinterpret_cast<char *>(dst);
        std::ifstream stream("/dev/urandom", std::ios_base::binary | std::ios_base::in);
        stream.read(buffer, dstlen);

        return dstlen;
    }

    template<class Table>
    __attribute__((noinline)) auto time_lookups(const Table *wrap_filter, const std::vector<u64> *element_set, size_t start, size_t end) -> ulong {
        static volatile bool dummy;
        bool x = 0;

        auto t0 = std::chrono::high_resolution_clock::now();
        for (size_t i = start; i < end; ++i) {
            x ^= FilterAPI<Table>::Contain(element_set->at(i), wrap_filter);
            // #if PREVENT_PIPELINING
            // asm volatile(
            //         "rdtscp\n\t"
            //         "lfence"
            //         :
            //         :
            //         : "rax", "rcx", "rdx", "memory");
            // #endif
        }
        auto t1 = std::chrono::high_resolution_clock::now();

        dummy = x;
        return std::chrono::duration_cast<ns>(t1 - t0).count();
    }

    template<class Table>
    __attribute__((noinline)) auto time_insertions(Table *wrap_filter, const std::vector<u64> *element_set, size_t start, size_t end) -> ulong {
        auto t0 = std::chrono::steady_clock::now();
        for (size_t i = start; i < end; ++i) {
            FilterAPI<Table>::Add(element_set->at(i), wrap_filter);
            // #if PREVENT_PIPELINING
            // asm volatile(
            //         "rdtscp\n\t"
            //         "lfence"
            //         :
            //         :
            //         : "rax", "rcx", "rdx", "memory");
            // #endif
        }
        // FilterAPI<Table>::AddAll(*element_set, start, end, wrap_filter);
        auto t1 = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<ns>(t1 - t0).count();
    }

    /*template<class Table>
    __attribute__((noinline)) auto time_lookups_perf(const Table *wrap_filter, const std::vector<u64> *element_set, size_t start, size_t end, std::stringstream &ss, bool with_header) -> ulong {
        static volatile bool dummy;
        bool x = 0;

        PerfEvent e;
        asm volatile(
                "rdtscp\n\t"
                "lfence"
                :
                :
                : "rax", "rcx", "rdx", "memory");
        e.startCounters();
        // auto t0 = std::chrono::high_resolution_clock::now();
        for (size_t i = start; i < end; ++i) {
            x ^= FilterAPI<Table>::Contain(element_set->at(i), wrap_filter);
            // #if PREVENT_PIPELINING
            // asm volatile(
            //         "rdtscp\n\t"
            //         "lfence"
            //         :
            //         :
            //         : "rax", "rcx", "rdx", "memory");
            // #endif
        }
        // auto t1 = std::chrono::high_resolution_clock::now();
        dummy = x;
        e.stopCounters();
        asm volatile(
                "rdtscp\n\t"
                "lfence"
                :
                :
                : "rax", "rcx", "rdx", "memory");
        if (with_header) {
            e.printReport(ss, end - start);
        } else {
            e.printReport_NoHeader(ss, end - start);
        }
        // return std::chrono::duration_cast<ns>(t1 - t0).count();
        return e.get_time();
    }

     template<class Table>
    __attribute__((noinline)) auto time_insertions_perf(Table *wrap_filter, const std::vector<u64> *element_set, size_t start, size_t end, std::stringstream &ss) -> ulong {
        PerfEvent e;
        e.startCounters();
        // auto t0 = std::chrono::high_resolution_clock::now();
        for (size_t i = start; i < end; ++i) {
            FilterAPI<Table>::Add(element_set->at(i), wrap_filter);
            // #if PREVENT_PIPELINING
            // asm volatile(
            //         "rdtscp\n\t"
            //         "lfence"
            //         :
            //         :
            //         : "rax", "rcx", "rdx", "memory");
            // #endif
        }
        // FilterAPI<Table>::AddAll(*element_set, start, end, wrap_filter);
        // auto t1 = std::chrono::high_resolution_clock::now();
        e.stopCounters();
        asm volatile(
                "rdtscp\n\t"
                "lfence"
                :
                :
                : "rax", "rcx", "rdx", "memory");
        if (start == 0) {
            e.printReport(ss, end - start);
        } else {
            e.printReport_NoHeader(ss, end - start);
        }
        // return std::chrono::duration_cast<ns>(t1 - t0).count();
        return e.get_time();
        // return std::chrono::duration_cast<ns>(t1 - t0).count();
    }
 */
    template<class Table>
    __attribute__((noinline)) auto time_deletions(Table *wrap_filter, const std::vector<u64> *element_set, size_t start, size_t end) -> ulong {
        if (!(FilterAPI<Table>::get_functionality(wrap_filter) & 4)) {
            //FIXME: UNCOMMENT!
            std::cout << FilterAPI<Table>::get_name(wrap_filter) << " does not support deletions." << std::endl;
            return 0;
        }

        auto t0 = std::chrono::steady_clock::now();
        for (size_t i = start; i < end; ++i) {
            FilterAPI<Table>::Remove(element_set->at(i), wrap_filter);
        }
        auto t1 = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<ns>(t1 - t0).count();
    }

    template<class Table>
    void benchmark_single_round_np_incremental(Table *wrap_filter, const std::vector<u64> *add_vec, const std::vector<u64> *find_vec, size_t round_counter, size_t benchmark_precision, u64 *data, bool to_print, std::stringstream &add_ss, std::stringstream &uni_find_ss, std::stringstream &yes_find_ss) {
        /* switch between the commented lines with "time_time_insertions" and "time_insertions_perf" to also get a csv file with data on the performances counters.*/
        const size_t find_step = find_vec->size() / benchmark_precision;
        size_t add_step = add_vec->size() / benchmark_precision;
        const size_t true_find_step = add_step;
        size_t add_start = round_counter * add_step;
        

        auto insertion_time = time_insertions(wrap_filter, add_vec, add_start, add_start + add_step);
        // auto insertion_time = time_insertions_perf(wrap_filter, add_vec, add_start, add_start + add_step, add_ss);
        asm volatile(
                "rdtscp\n\t"
                "lfence"
                :
                :
                : "rax", "rcx", "rdx", "memory");
        auto find_start = find_step * round_counter;
        auto uniform_lookup_time = time_lookups(wrap_filter, find_vec, find_start, find_start + find_step);
        // auto uniform_lookup_time = time_lookups_perf(wrap_filter, find_vec, find_start, find_start + find_step, uni_find_ss, round_counter == 0);
        asm volatile(
                "rdtscp\n\t"
                "lfence"
                :
                :
                : "rax", "rcx", "rdx", "memory");
        // auto true_lookup_time = time_lookups(wrap_filter, add_vec, del_start, del_start + add_step);
        std::vector<u64> temp_vec;
        fill_vec_by_samples(0, add_start + add_step, true_find_step, add_vec, &temp_vec);
        asm volatile(
                "rdtscp\n\t"
                "lfence"
                :
                :
                : "rax", "rcx", "rdx", "memory");
        auto true_lookup_time = time_lookups(wrap_filter, &temp_vec, 0, true_find_step);
        // auto true_lookup_time = time_lookups_perf(wrap_filter, &temp_vec, 0, true_find_step, yes_find_ss, round_counter == 0);
        asm volatile(
                "rdtscp\n\t"
                "lfence"
                :
                :
                : "rax", "rcx", "rdx", "memory");

        size_t index = 4 * (round_counter);
        data[index + 0] = insertion_time;
        data[index + 1] = uniform_lookup_time;
        data[index + 2] = true_lookup_time;
        data[index + 3] = 0;

        if (to_print) {
            constexpr size_t width = 12;
            std::cout << round_counter << ": \t";
            std::cout << std::setw(width) << std::left << ((1.0 * add_step) / (1.0 * data[index + 0] / 1e9)) << ", ";
            std::cout << std::setw(width) << std::left << ((1.0 * find_step) / (1.0 * data[index + 1] / 1e9)) << ", ";
            std::cout << std::setw(width) << std::left << ((1.0 * add_step) / (1.0 * data[index + 2] / 1e9)) << ", ";
            std::cout << std::setw(width) << std::left << ((1.0 * add_step) / (1.0 * data[index + 3] / 1e9)) << std::endl;
        }
    }

    template<typename Table>
    void Bench_res_to_file_incremental_22(size_t filter_max_capacity, size_t bench_precision, const std::vector<u64> *add_vec, const std::vector<u64> *lookup_vec, std::string file_prefix, bool to_print = false) {
        const size_t data_size = (bench_precision + 2) * 4;
        assert(data_size < 1024);
        u64 data[data_size];
        std::fill(data, data + data_size, 0);
        // std::cout << "H0!"  << std::endl;
        auto t0 = std::chrono::high_resolution_clock::now();
        Table filter = FilterAPI<Table>::ConstructFromAddCount(filter_max_capacity);
        auto t1 = std::chrono::high_resolution_clock::now();
        // std::cout << "Here!"  << std::endl;
        auto init_time = std::chrono::duration_cast<ns>(t1 - t0).count();
        Table *wrap_filter = &filter;

        std::string filter_name = FilterAPI<Table>::get_name(wrap_filter);
        if (to_print)
            std::cout << "filter_name: " << filter_name << std::endl;
        // benchmark_round0_np(wrap_filter, add_vec, lookup_vec, bench_precision, rng, data, to_print);

        if (to_print) std::cout << std::string(80, '=') << std::endl;
        std::stringstream add_ss, uni_find_ss, yes_find_ss;
        for (size_t round = 0; round < bench_precision; ++round) {
            benchmark_single_round_np_incremental(wrap_filter, add_vec, lookup_vec, round, bench_precision, data, to_print, add_ss, uni_find_ss, yes_find_ss);
            // if (to_print) std::cout << std::string(80, '=') << std::endl;
            asm volatile(
                    "rdtscp\n\t"
                    "lfence"
                    :
                    :
                    : "rax", "rcx", "rdx", "memory");
        }
        if (to_print) std::cout << std::string(80, '=') << std::endl;

        //UNCOMMENT me to get CSV files.
        // write_perf_res_to_file(wrap_filter, init_time, filter_max_capacity, lookup_vec->size(), bench_precision, data, file_prefix, add_ss, uni_find_ss, yes_find_ss);
        write_res_to_file_core(wrap_filter, init_time, filter_max_capacity, lookup_vec->size(), bench_precision, data, file_prefix);

        if (to_print)
            FPR_test0_after_build(wrap_filter, add_vec, lookup_vec, bench_precision);
    }

    template<typename Table>
    void write_perf_res_to_file(const Table *wrap_filter, size_t init_time, size_t filter_max_capacity, size_t lookup_reps, size_t bench_precision, const u64 *data, std::string file_prefix, std::stringstream &add_ss, std::stringstream &uni_find_ss, std::stringstream &yes_find_ss) {
        std::string filter_name = FilterAPI<Table>::get_name(wrap_filter);
        std::string file_name = file_prefix + filter_name + ".csv";
        std::cout << "file_name: " << file_name << std::endl;
        std::fstream file(file_name, std::fstream::in | std::fstream::out | std::fstream::app);
        // file << std::endl;
        // std::fstream file(file_name, std::fstream::in | std::fstream::out | std::fstream::trunc);
        // std::string str_add = add_ss.str();
        // std::cout << std::string(80, '@') << std::endl;
        // std::cout << str_add << std::endl;
        // std::cout << std::string(80, '@') << std::endl;
        file << "add, uni_find, yes_find" << std::endl;
        for (size_t i = 0; i < bench_precision + 1; i++) {
            std::string temp[3];
            std::getline(add_ss, temp[0]);
            std::getline(uni_find_ss, temp[1]);
            std::getline(yes_find_ss, temp[2]);
            file << temp[0] << ",";
            file << temp[1] << ",";
            file << temp[2] << std::endl;
        }
        file.close();
    }

    template<typename Table>
    void write_res_to_file_core(const Table *wrap_filter, size_t init_time, size_t filter_max_capacity, size_t lookup_reps, size_t bench_precision, const u64 *data, std::string file_prefix) {
        std::string filter_name = FilterAPI<Table>::get_name(wrap_filter);
        std::string file_name = file_prefix + filter_name;
        std::cout << "file_name: " << file_name << std::endl;
        std::fstream file(file_name, std::fstream::in | std::fstream::out | std::fstream::app);
        file << endl;
        // std::fstream file(file_name, std::fstream::in | std::fstream::out | std::fstream::trunc);
        file << "# This is a comment." << std::endl;
        file << "NAME\t" << filter_name << std::endl;
        file << "INIT_TIME(NANO_SECOND)\t" << init_time << std::endl;
        file << "FILTER_MAX_CAPACITY\t" << filter_max_capacity << std::endl;
        file << "BYTE_SIZE\t" << FilterAPI<Table>::get_byte_size(wrap_filter) << std::endl;
        file << "NUMBER_OF_LOOKUP\t" << lookup_reps << std::endl;
        file << std::endl;
        file << "# add, uniform lookup, true_lookup, deletions. Each columns unit is in nano second." << std::endl;
        file << std::endl;
        file << "BENCH_START" << std::endl;
        for (size_t i = 0; i < bench_precision; i++) {
            size_t index = i * 4;
            file << data[index];
            for (size_t j = 1; j < 4; j++) {
                file << ", " << data[index + j];
            }
            file << std::endl;
        }
        file << std::endl;
        file << "BENCH_END" << std::endl;
        file << "END_OF_FILE!" << std::endl;
        file.close();
    }

    template<typename Table>
    void write_build_res_to_file(const Table *wrap_filter, size_t init_time, size_t built_time, size_t filter_max_capacity, std::string file_prefix) {
        std::string filter_name = FilterAPI<Table>::get_name(wrap_filter);
        std::string file_name = file_prefix + filter_name;
        std::cout << "file_name: " << file_name << std::endl;
        std::fstream file(file_name, std::fstream::in | std::fstream::out | std::fstream::app);
        file << endl;
        // std::fstream file(file_name, std::fstream::in | std::fstream::out | std::fstream::trunc);
        file << "# This is a comment." << std::endl;
        file << "# Results for build." << std::endl;
        file << "NAME\t" << filter_name << std::endl;
        file << "INIT_TIME(NANO_SECOND)\t" << init_time << std::endl;
        file << "BUILT_TIME(NANO_SECOND)\t" << built_time << std::endl;
        file << "FILTER_MAX_CAPACITY(Actually-number-of-items-in-the-filter)\t" << filter_max_capacity << std::endl;
        file << "BYTE_SIZE\t" << FilterAPI<Table>::get_byte_size(wrap_filter) << std::endl;
        file << std::endl;
        file << "END_OF_FILE!" << std::endl;
        file.close();
    }


    template<typename Table>
    void bench_build_to_file(size_t filter_max_capacity, const std::vector<u64> *v_add, std::string file_prefix) {
        auto t0 = std::chrono::high_resolution_clock::now();
        Table filter = FilterAPI<Table>::ConstructFromAddCount(filter_max_capacity);
        auto t1 = std::chrono::high_resolution_clock::now();
        auto init_time = std::chrono::duration_cast<ns>(t1 - t0).count();
        Table *wrap_filter = &filter;
        std::string filter_name = FilterAPI<Table>::get_name(wrap_filter);
        auto is_static = (FilterAPI<Table>::get_functionality(wrap_filter) == 1);
        auto built_time = time_insertions(wrap_filter, v_add, 0, v_add->size());
        write_build_res_to_file(wrap_filter, init_time, built_time, filter_max_capacity, file_prefix);
    }

    template<typename Table>
    size_t bench_build_to_file22(size_t filter_max_capacity, const std::vector<u64> *v_add) {
        auto t0 = std::chrono::high_resolution_clock::now();
        Table filter = FilterAPI<Table>::ConstructFromAddCount(filter_max_capacity);
        auto t1 = std::chrono::high_resolution_clock::now();
        auto init_time = std::chrono::duration_cast<ns>(t1 - t0).count();
        Table *wrap_filter = &filter;
        // std::string filter_name = FilterAPI<Table>::get_name(wrap_filter);
        // auto is_static = (FilterAPI<Table>::get_functionality(wrap_filter) == 1);
        auto built_time = time_insertions(wrap_filter, v_add, 0, v_add->size());
        return built_time;
    }
    //////////////////////////////////////////////////////////////// ////////////////////////////////////////////////////////////////
    //////////////////////////////////////////////////////////////// ////////////////////////////////////////////////////////////////
    //////////////////////////////////////////////////////////////// ////////////////////////////////////////////////////////////////

    /* template<class Table, typename ItemType>
size_t count_finds(Table *wrap_filter, const std::vector<ItemType> *vec) {
    size_t counter = 0;
    for (size_t i = 0; i < vec->size(); i++) { counter += FilterAPI<Table>::Contain(vec->at(i), wrap_filter);
    }
    return counter;
} */

    template<class Table>
    size_t count_finds(const Table *wrap_filter, const std::vector<u64> *vec) {
        size_t counter = 0;
        for (size_t i = 0; i < vec->size(); i++) { counter += FilterAPI<Table>::Contain(vec->at(i), wrap_filter); }
        return counter;
    }

    /**
 * @brief Get the FPR (false positive probability) test0 object.
 * 
 * @tparam Table 
 * @param wrap_filter 
 * @param yes_vec 
 * @return std::tuple<size_t, size_t> 
 */
    template<class Table>
    size_t get_FPR_test0(Table *wrap_filter, const std::vector<u64> *v_add, const std::vector<u64> *v_find, bool sorted, std::vector<u64> *v_check, bool is_filled) {
        /**Insertion*/
        if (!is_filled) {
            FilterAPI<Table>::Fill(v_add, wrap_filter, sorted);
        }
        size_t counter = 0;
        hashing::TwoIndependentMultiplyShift H0 = *wrap_filter->GetH0();
        auto name = FilterAPI<Table>::get_name(wrap_filter);
        for (auto el : *v_check) {
            // std::cout << "Filter (" << name << ") check " << el << " " << "H0(el)=" << H0(el) << " ." << std::endl;
            // std::cout << "counter: \t" << counter << std::endl;
            counter++;
            if (!FilterAPI<Table>::Contain(el, wrap_filter)) {
                std::cout << "Filter (" << name << ") has2 a false negative " << el << " " << "H0(el)=" << H0(el) << " . exiting." << std::endl;
                std::cout << "bucket_index=" << wrap_filter->reduce32(H0(el) >> 32, wrap_filter->GetNumPd()) << " val=" << wrap_filter->fixed_reduce(H0(el)) << std::endl;
                std::cout << "counter: \t" << counter << std::endl;
                FilterAPI<Table>::Contain(el, wrap_filter);
                assert(0);
                exit(-42);
            }
        }

        size_t yes_res = count_finds(wrap_filter, v_find);
        return yes_res;
    }

    template<class Table>
    void FPR_test0_after_build(const Table *wrap_filter, const std::vector<u64> *v_add, const std::vector<u64> *v_find, size_t bench_precision) {
        // Table filter = FilterAPI<Table>::ConstructFromAddCount(v_add->size());
        // Table *wrap_filter = &filter;
        size_t counter = 0;
        const size_t lim = v_add->size() / bench_precision * bench_precision;
        for (auto el : *v_add) {
            counter++;
            if (counter >= lim)
                break;
            if (!FilterAPI<Table>::Contain(el, wrap_filter)) {
                auto name = FilterAPI<Table>::get_name(wrap_filter);
                std::cout << "Filter (" << name << ") has3 a false negative. exiting." << std::endl;
                std::cout << "counter: \t" << counter << std::endl;
                FilterAPI<Table>::Contain(el, wrap_filter);
                assert(0);
                exit(-42);
            }
        }

        size_t yes_res = count_finds(wrap_filter, v_find);
        // auto yes_res = get_FPR_test0(wrap_filter, v_add, v_find);

        auto s = FPR_parse_data_str_22(wrap_filter, v_add, v_find, yes_res);
        std::cout << "s:\n"
                  << s << std::endl;
        // FPR_printer(wrap_filter, v_add, v_find, yes_res);
    }

    template<class Table>
    std::string FPR_parse_data_str_22(const Table *wrap_filter, const std::vector<u64> *v_add, const std::vector<u64> *v_find, size_t yes_res) {
        size_t true_counter = yes_res;
        // size_t true_counter = yes_res;
        assert(true_counter <= v_find->size());
        size_t false_counter = v_find->size() - yes_res;
        const size_t filter_max_capacity = v_add->size();
        const size_t filter_true_cap = FilterAPI<Table>::get_cap(wrap_filter);
        auto filter_id = FilterAPI<Table>::get_ID(wrap_filter);
        bool skip = (filter_id == BBF) or (filter_id == SIMD_fixed);
        if ((!skip) and (filter_max_capacity != filter_true_cap)) {
            std::cout << "filter_name:     \t" << FilterAPI<Table>::get_name(wrap_filter) << std::endl;
            std::cout << "filter_max_cap:  \t" << filter_max_capacity << std::endl;
            std::cout << "filter_true_cap: \t" << filter_true_cap << std::endl;
        }
        std::string filter_name = FilterAPI<Table>::get_name(wrap_filter);
        const size_t filter_byte_size = FilterAPI<Table>::get_byte_size(wrap_filter);
        double positive_ratio = 1.0 * true_counter / (false_counter + true_counter);
        double bpi = filter_byte_size * 8.0 / filter_max_capacity;
        double optimal_bits_for_err = -log2(positive_ratio);
        double bpi_diff = (bpi - optimal_bits_for_err);
        double bpi_ratio = (bpi / optimal_bits_for_err);
        double fpp_mult_factor = positive_ratio / (1.0 / 256);
        double data[] = {positive_ratio, bpi, optimal_bits_for_err, bpi_diff, bpi_ratio, fpp_mult_factor};
        std::stringstream ss;
        ss << std::setw(34) << std::left << filter_name << "\t, " << std::setw(12) << std::left << filter_byte_size;
        for (auto &&x : data) { ss << ", " << std::setw(12) << std::left << x; }
        ss << std::endl;
        std::string line = ss.str();
        return line;
    }

    template<class Table>
    std::string FPR_test_as_str(Table *wrap_filter, const std::vector<u64> *v_add, const std::vector<u64> *v_find, bool sorted, std::vector<u64> *v_check, bool is_filled) {
        auto yes_res = get_FPR_test0(wrap_filter, v_add, v_find, sorted, v_check, is_filled);
        size_t true_counter = yes_res;
        assert(true_counter <= v_find->size());
        size_t false_counter = v_find->size() - yes_res;
        const size_t filter_max_capacity = v_add->size();
        const size_t filter_true_cap = FilterAPI<Table>::get_cap(wrap_filter);
        auto filter_id = FilterAPI<Table>::get_ID(wrap_filter);
        bool skip = (filter_id == BBF) or (filter_id == SIMD_fixed);
        if ((!skip) and (filter_max_capacity != filter_true_cap)) {
            // UNCOMMENT ME!!! FIXME!!!
            std::cout << "filter_name:     \t" << FilterAPI<Table>::get_name(wrap_filter) << std::endl;
            std::cout << "filter_max_cap:  \t" << filter_max_capacity << std::endl;
            std::cout << "filter_true_cap: \t" << filter_true_cap << std::endl;
        }
        std::string filter_name = FilterAPI<Table>::get_name(wrap_filter);
        const size_t filter_byte_size = FilterAPI<Table>::get_byte_size(wrap_filter);
        double positive_ratio = 1.0 * true_counter / (false_counter + true_counter);
        double bpi = filter_byte_size * 8.0 / filter_max_capacity;
        double optimal_bits_for_err = -log2(positive_ratio);
        double bpi_diff = (bpi - optimal_bits_for_err);
        double bpi_ratio = (bpi / optimal_bits_for_err);
        // double fpp_mult_factor = positive_ratio / (1.0 / 256);
        double data[] = {positive_ratio, bpi, optimal_bits_for_err, bpi_diff, bpi_ratio};
        std::stringstream ss;
        ss << std::setw(34) << std::left << filter_name << "\t, " << std::setw(12) << std::left << filter_byte_size;
        for (auto &&x : data) { ss << ", " << std::setw(12) << std::left << x; }
        ss << std::endl;
        std::string line = ss.str();
        return line;
        // std::cout << line;// << std::endl;
    }


    template<class Table>
    void FPR_test(std::vector<u64> *v_add, const std::vector<u64> *v_find, std::string path, bool is_hashable, bool create_file = false, bool sorted = false) {
        const size_t filter_max_capacity = v_add->size();
        Table filter = FilterAPI<Table>::ConstructFromAddCount(v_add->size(), false);

        FixedArray<min_pd::MAX_WITH_SPARE> *arr_buff = nullptr;
        int ok = posix_memalign(reinterpret_cast<void **>(&arr_buff), 32, 
                       (filter.GetNumPd() + 1) * sizeof(FixedArray<min_pd::MAX_WITH_SPARE>));
        
        if (ok != 0) {
            std::cout << "Failed!!!" << std::endl;
            exit(1);
        }

        memset(arr_buff, 0, (filter.GetNumPd() + 1) * sizeof(FixedArray<min_pd::MAX_WITH_SPARE>));

        std::cout << "sizeof=" << sizeof(FixedArray<min_pd::MAX_WITH_SPARE>) << "other=" << sizeof(__m256i) << " allocated=" << std::hex << arr_buff << std::endl;
        arr_buff += 1;
        std::cout << "moved=" << std::hex << arr_buff << std::dec << std::endl;
        std::cout << " num pd is " << filter.GetNumPd() << " allocated_size=" << sizeof(FixedArray<min_pd::MAX_WITH_SPARE>) * filter.GetNumPd() << std::endl;
        Table *wrap_filter = &filter;
        std::vector<u64> hashed_v_add;
        std::vector<u64> *v_to_add = v_add;
        std::vector<uint64_t> temp_arr_for_sort(v_to_add->size());
        // int pid = getpid();
        // int cpid = fork();
        // if(cpid == 0) {
        //     // child process .  Run your perf stat
        //     char buf[1024];
        //     // sprintf(buf, "/opt/intel/oneapi/vtune/latest/bin64/vtune -collect system-overview -target-pid %d", pid);
        //     sprintf(buf, "perf stat  -e L1-dcache-loads,L1-dcache-load-misses -p %d", pid);
        //     execl("/bin/sh", "sh", "-c", buf, NULL);
        // } else {
        //     setpgid(cpid, 0);
        //     sleep(1);

            #ifdef HASH_AHEAD
            if (is_hashable) {
                FilterAPI<Table>::HashVec(v_add, &hashed_v_add, wrap_filter);
                v_to_add = &hashed_v_add;
            }
            #endif
            if (sorted) {
                std::fill(temp_arr_for_sort.begin(), temp_arr_for_sort.end(), 0);
                FilterAPI<Table>::Sort(*v_to_add, wrap_filter, temp_arr_for_sort);
            }
            auto start_time = std::chrono::high_resolution_clock::now();
            auto line = FPR_test_as_str(wrap_filter, v_to_add, v_find, sorted, v_add);
            auto end_time = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> elapsed = end_time - start_time;
            std::cout << "Time taken: " << elapsed.count() << " seconds" << std::endl;
            std::cout << "Per entry time: " 
                      << (std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count() / static_cast<double>(filter.get_cap())) 
                      << " ns" << std::endl;
        //     kill(cpid, 9);
        // }

        // std::fstream file(path, std::fstream::in | std::fstream::out | std::fstream::app);
        // file << line;
        // file.close();

        // std::cout << line << std::endl;
        track_time(FilterAPI<Table>::get_fill_time_per_entry_ns());
    }

    class FileReader {
    public:
        FileReader(const std::string& path, uint64_t vec_size)
            : _path(path), _vec_size(vec_size), _offset(0) {
            // Open the file for reading in binary mode
            _inFile.open(_path, std::ios::binary);
            if (!_inFile) {
                std::cerr << "Failed to open file: " << _path << std::endl;
                throw std::ios_base::failure("Failed to open file");
            }
        }

        ~FileReader() {
            if (_inFile.is_open()) {
                _inFile.close();
            }
        }

        // Read the next chunk of data into the provided buffer.
        // Returns the number of entries read (0 means no more data).
        size_t readNextChunk(std::vector<uint64_t>& buffer, uint64_t offset) {
            // std::fill(buffer.begin(), buffer.end(), 0);
            _inFile.seekg(_offset * sizeof(uint64_t), std::ios::beg);
            _inFile.read(reinterpret_cast<char*>(buffer.data() + offset), _vec_size * sizeof(uint64_t));
            size_t entriesRead = _inFile.gcount() / sizeof(uint64_t);

            // If no data is read, return 0 to indicate the end of the file
            if (entriesRead == 0) return 0;

            // Update the offset for the next read
            _offset += _vec_size;
            return entriesRead;
        }

        std::string _path;         // File path
        uint64_t _vec_size;        // The size of each chunk (in number of uint64_t elements)
        uint64_t _offset;          // The current offset in the file
        std::ifstream _inFile;     // File input stream
    };

    template<class Table>
    void hash_and_merge_filters(std::string path, uint64_t vec_size, Table* filter) {
        // Create a FileReader to handle file reading in chunks
        FileReader fileReader(path, vec_size);

        // Temporary buffers for data processing
        std::vector<uint64_t> buffer(vec_size);
        std::vector<uint64_t> temp_arr_for_sort(vec_size);

        // Timer to track the total processing time
        std::chrono::duration<double> duration_total{0.0};

        // Iterate through the file in chunks
        size_t iter = 0;
        while (true) {
    
            // Process the buffer
            std::fill(temp_arr_for_sort.begin(), temp_arr_for_sort.end(), 0); // Clear the sort buffer
            auto start_time = std::chrono::high_resolution_clock::now();

            // Read the next chunk of data
            // size_t entriesRead = fileReader.readNextChunk(buffer);
            
            // Exit if no more data is available
            // if (entriesRead == 0) break;

            // Hash and sort the data
            FilterAPI<Table>::HashVecInPlace(&buffer, filter);
            FilterAPI<Table>::Sort(buffer, filter, temp_arr_for_sort);
            FilterAPI<Table>::FillAndMerge(buffer, filter);

            auto end_time = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> elapsed = end_time - start_time;
            duration_total += elapsed;

            iter++;
        }
        std::cout << "Reading from drive and merging filters took: " << duration_total.count() << " s" << std::endl;
    }

#include <sys/mman.h>  // For mmap(), munmap(), madvise()
#include <fcntl.h>     // For open()
#include <unistd.h>    // For close()
#include <sys/stat.h>  // For fstat()

template<class Table>
uint64_t hash_file_ahead(std::string path, uint64_t vec_size, Table* filter) {
    std::chrono::duration<double> duration_total{0.0};
    std::chrono::duration<double> duration_read{0.0};
    std::chrono::duration<double> duration_write{0.0};
    std::chrono::duration<double> duration_hash{0.0};
    std::chrono::duration<double> duration_sort{0.0};

    std::remove("file");

    // Open input file
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        std::cerr << "Failed to open file" << std::endl;
        return 0;
    }

    // Get file size
    struct stat st;
    if (fstat(fd, &st) < 0) {
        std::cerr << "Failed to get file size" << std::endl;
        close(fd);
        return 0;
    }
    size_t file_size = st.st_size;

    // Open output file
    int out_fd = open("file", O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
    if (out_fd < 0) {
        std::cerr << "Failed to open output file" << std::endl;
        close(fd);
        return 0;
    }

    std::cout << "allocating " << file_size << " page size is" << sysconf(_SC_PAGE_SIZE) << std::endl;

    if (posix_fallocate(out_fd, 0, file_size) != 0) {
        std::cerr << "Failed to preallocate output file space" << std::endl;
        close(fd);
        close(out_fd);
        return 0;
    }

    auto start_time = std::chrono::high_resolution_clock::now();

    std::vector<uint64_t> buffer_vec(vec_size);
    std::vector<uint64_t> temp_arr_for_sort(vec_size);

    // Memory-map the entire file
    void* mapped_data = mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (mapped_data == MAP_FAILED) {
        std::cerr << "mmap failed" << std::endl;
        close(fd);
        close(out_fd);
        return 0;
    }


    uint64_t num_groups = 0;

    uint64_t* data = static_cast<uint64_t*>(mapped_data);
    size_t num_entries = file_size / sizeof(uint64_t);

    for (size_t i = 0; i < num_entries; i += vec_size) {
        madvise(static_cast<uint64_t*>(mapped_data) + i * vec_size, vec_size * sizeof(uint64_t), MADV_SEQUENTIAL);

        // Hashing
        auto start_hash_time = std::chrono::high_resolution_clock::now();
        FilterAPI<Table>::HashBufInVec(data + i, filter, buffer_vec, vec_size);
        auto end_hash_time = std::chrono::high_resolution_clock::now();
        duration_hash += end_hash_time - start_hash_time;

        // Sorting
        auto start_sort_time = std::chrono::high_resolution_clock::now();
        FilterAPI<Table>::Sort(buffer_vec, filter, temp_arr_for_sort);
        auto end_sort_time = std::chrono::high_resolution_clock::now();
        duration_sort += end_sort_time - start_sort_time;

        // Writing
        auto start_write_time = std::chrono::high_resolution_clock::now();
        ssize_t bytes_written = write(out_fd, buffer_vec.data(), buffer_vec.size() * sizeof(uint64_t));
        auto end_write_time = std::chrono::high_resolution_clock::now();
        duration_write += end_write_time - start_write_time;
        num_groups++;

        if (bytes_written < 0) {
            std::cerr << "Error occurred while writing to the output file" << std::endl;
            break;
        }
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    duration_total += end_time - start_time;

    munmap(mapped_data, file_size);
    close(out_fd);
    close(fd);

    std::cout << "Total processing time: " << duration_total.count() << " s" << std::endl;
    std::cout << "Reading time: " << duration_read.count() << " s" << std::endl;
    std::cout << "Hashing time: " << duration_hash.count() << " s" << std::endl;
    std::cout << "Sorting time: " << duration_sort.count() << " s" << std::endl;
    std::cout << "Writing time: " << duration_write.count() << " s" << std::endl;
    return num_groups;
}

template<class Table>
uint64_t improved_hash_sort(std::string path, uint64_t vec_size, Table* filter, typename FilterAPI<Table>::StreamBuffer *arr_buf, size_t num_entries) {
    std::chrono::duration<double> duration_total{0.0};
    std::chrono::duration<double> duration_read{0.0};
    std::chrono::duration<double> duration_hash{0.0};
    std::chrono::duration<double> duration_sort{0.0};
    std::chrono::duration<double> duration_fill{0.0};

    std::remove("file");

    std::cout << "vec_size=" << vec_size << " num_entries=" << num_entries << std::endl;
    std::vector<uint64_t> buffer_vec(vec_size);
    std::vector<uint64_t> tmp(vec_size);
    // BCF keeps one backyard lock set across every hash chunk and the later front-yard fill.
    // Prefix Filter's state is empty, so the common streaming driver pays nothing for it.
    typename FilterAPI<Table>::StreamingBuildState build_state(filter);
    FileReader fileReader(path, 1024);

    uint64_t num_groups = 0;
    size_t total_num_entries_read = 0;
    uint64_t cur_num_entries_read = 0;
    // PerfManager mgr;
    // mgr.start();
    auto start_time = std::chrono::high_resolution_clock::now();

    while (true) {
        // mgr.pause();
        while (cur_num_entries_read != vec_size) {
            auto start_read_time = std::chrono::high_resolution_clock::now();
            size_t entriesRead = fileReader.readNextChunk(buffer_vec, cur_num_entries_read);
            auto end_read_time = std::chrono::high_resolution_clock::now();
            duration_read += end_read_time - start_read_time;
            
            if (entriesRead == 0) break;
            cur_num_entries_read += entriesRead;
            total_num_entries_read += entriesRead;
        }
        if (cur_num_entries_read == 0) break;
        cur_num_entries_read = 0;
        // mgr.resume();

        auto start_hash_time = std::chrono::high_resolution_clock::now();
        FilterAPI<Table>::ImprovedHashVec(
                &buffer_vec, arr_buf, filter, tmp, num_entries, build_state);
        auto end_hash_time = std::chrono::high_resolution_clock::now();
        duration_hash += end_hash_time - start_hash_time;

        if (total_num_entries_read >= num_entries) {
            break;
        }
        //  else if (total_num_entries_read >= num_entries) {
        //     std::cout << "read too many entries! read=" << total_num_entries_read << " num=" << num_entries << std::endl;
        //     exit(1);
        // }
    }

    // auto start_sort_time = std::chrono::high_resolution_clock::now();
    // FilterAPI<Table>::ImprovedSort(arr_buf, filter);
    // auto end_sort_time = std::chrono::high_resolution_clock::now();
    // duration_sort += end_sort_time - start_sort_time;

    auto start_fill_time = std::chrono::high_resolution_clock::now();
    FilterAPI<Table>::FillWithBuffer(arr_buf, filter, build_state);
    auto end_fill_time = std::chrono::high_resolution_clock::now();
    duration_fill += end_fill_time - start_fill_time;

    auto total = duration_fill + duration_hash;
    auto total_per_entry = compute_fill_time_per_entry_ns(total.count()* 1000000000, num_entries);
    std::cout << "total_per_entry=" << total_per_entry << " total=" << total.count()* 1000000000 << " num_entries=" << num_entries << std::endl;
    track_time(total_per_entry);
    auto end_time = std::chrono::high_resolution_clock::now();
    duration_total += end_time - start_time;
    // mgr.stop();

    // std::cout << "Total processing time: " << duration_total.count() << " s" << std::endl;
    // std::cout << "Total important time: " << duration_hash.count() + duration_fill.count() << " s" << std::endl;
    // std::cout << "Total important time per entry: " << (((duration_hash.count() + duration_fill.count()) / num_entries) * 1000000000) << " ns" << std::endl;
    std::cout << "Reading time: " << duration_read.count() << " s" << std::endl;
    std::cout << "Hashing time: " << duration_hash.count() << " s" << std::endl;
    std::cout << "Sorting time: " << duration_sort.count() << " s" << std::endl;
    std::cout << "Filling time: " << duration_fill.count() << " s" << std::endl;
    // track_time(FilterAPI<Table>::get_fill_time_per_entry_ns());
    return num_groups;
}

    template<class Table>
    void FillWithFile(uint64_t read_size, std::string path, Table *wrap_filter) {
        std::vector<u64> buffer(read_size);

        auto start_time = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> duration_fill{0.0};
        FileReader file_reader(path, read_size);
        
        while (file_reader.readNextChunk(buffer, 0)) {
            auto start_time_fill = std::chrono::high_resolution_clock::now();
            FilterAPI<Table>::Fill(&buffer, wrap_filter, false, false);
            auto end_time_fill = std::chrono::high_resolution_clock::now();
            duration_fill += end_time_fill - start_time_fill;
        }
        auto end_time = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> elapsed = end_time - start_time;
        std::cout << std::setw(20) << "Elapsed time:" << std::setw(10) << elapsed.count() << " s" << std::endl;
        std::cout << std::setw(20) << "Elapsed Fill time:" << std::setw(10) << duration_fill.count() << " s" << std::endl;
    }

    template<class Table>
    void FPR_test_with_file(std::string path, bool is_hashable, size_t num_entries, size_t read_size, size_t max_capacity, bool create_file = false) {
        Table filter = FilterAPI<Table>::ConstructFromAddCount(max_capacity, !is_hashable);

        // One key buffer per bucket, plus one buffer of slack in front: the filter's first level is
        // built in place over these buffers, starting one buffer behind them.
        using StreamBuffer = typename FilterAPI<Table>::StreamBuffer;
        StreamBuffer *arr_buff = nullptr;
        if (is_hashable) {
            const size_t num_buckets = FilterAPI<Table>::GetNumBuckets(&filter);
            const size_t buffers_bytes = (num_buckets + 1) * sizeof(StreamBuffer);
            int ok = posix_memalign(reinterpret_cast<void **>(&arr_buff), 64, buffers_bytes);
            if (ok != 0) {
                std::cout << "Failed!!!" << std::endl;
                exit(1);
            }

            std::cout << "allocated=" << buffers_bytes << std::endl;

            memset(arr_buff, 0, buffers_bytes);
            arr_buff += 1;
        }
        


        Table *wrap_filter = &filter;
        std::vector<u64> hashed_v_add;
        // static const uint64_t vec_size = 1 << 27; // chunk size is vec_size * 8.
        // static const uint64_t read_size = 1 << 20;
        if (is_hashable) {
            std::cout << "Running improved_hash_sort" << std::endl;
            improved_hash_sort(path, read_size, &filter, arr_buff, num_entries);

            if (std::getenv("TMPCHECK_VERIFY_KEYS") != nullptr) {
                FileReader reader(path, read_size);
                std::vector<uint64_t> buffer(read_size);
                size_t false_negatives = 0;
                size_t checked = 0;

                while (checked < num_entries) {
                    const size_t entries_read = reader.readNextChunk(buffer, 0);
                    if (entries_read == 0) {
                        throw std::logic_error(
                                "Key verification reached end of file before all inserted keys");
                    }

                    const size_t entries_to_check =
                            std::min(entries_read, num_entries - checked);
                    for (size_t i = 0; i < entries_to_check; ++i) {
                        false_negatives += !FilterAPI<Table>::Contain(buffer[i], wrap_filter);
                    }
                    checked += entries_to_check;
                }

                std::cout << "[verify] checked=" << checked
                          << " false_negatives=" << false_negatives << std::endl;
                if (false_negatives != 0) {
                    throw std::logic_error(
                            "Bulk construction key verification found false negatives");
                }
            }
            
            free(arr_buff - 1);
            return;
        //     uint64_t num_groups = hash_file_ahead(path, vec_size, &filter);
        //     std::cout << "num_groups=" << num_groups << std::endl;
        //     std::vector<NumWithInd> preallocated_vec(num_groups);
        //     SortedIterator sorted_iter("file", num_groups * vec_size * sizeof(uint64_t), 1024 * 1024, preallocated_vec, num_groups);
        //     uint64_t num = 0;
        //     uint64_t iter = 0;
        //     auto start_time = std::chrono::high_resolution_clock::now();
        //     while (!sorted_iter.is_done()) {
        //         uint64_t next_num = sorted_iter.next();
        //         if (next_num < num) {
        //             std::cout << "UNORDERED num=" << num << " next_num=" << next_num << " iter=" << iter << std::endl;
        //             exit(1);
        //         }
        //         num = next_num;
        //         iter++;
        //     }
        //     std::cout << "num items = " << iter << "capacity=" << num_entries << std::endl;
        //     auto end_time = std::chrono::high_resolution_clock::now();
        //     std::chrono::duration<double> elapsed = end_time - start_time;
        //     std::cout << std::setw(20) << "Elapsed time running:" << std::setw(10) << elapsed.count() << " s" << std::endl;

        //     return;
        //     // hash_and_merge_filters(path, vec_size, &filter);
        //     // #ifdef HASH_AHEAD
        //     // if (is_hashable) {
        //     //     FilterAPI<Table>::HashVec(v_add, &hashed_v_add, wrap_filter);
        //     //     v_to_add = &hashed_v_add;
        //     // }
        //     // #endif
        //     // if (sorted) {
        //     //     FilterAPI<Table>::Sort(*v_to_add, wrap_filter);
        //     // }
        }

        FillWithFile(read_size, path, wrap_filter);
    }

    template<class Table>
    void profile_benchmark(Table *wrap_filter, const std::vector<const std::vector<u64> *> *elements) {
        auto add_vec = elements->at(0);
        auto find_vec = elements->at(1);
        auto delete_vec = elements->at(2);

        auto insertion_time = time_insertions(wrap_filter, add_vec, 0, add_vec->size());

        printf("insertions done\n");
        fflush(stdout);
        ulong uniform_lookup_time = 0;
        ulong true_lookup_time = 0;
        // size_t true_lookup_time = 0;
        char buf[1024];
        // sprintf(buf, "perf record -p %d &", getpid());
        // sprintf(buf, "perf stat -p %d -e cycles -e instructions -e cache-misses -e cache-references -e L1-dcache-load-misses -e L1-dcache-loads -e LLC-load-misses -e LLC-loads -e dTLB-load-misses -e dTLB-loads -e node-load-misses -e node-loads -e branches -e branch-misses &", getpid());
        sprintf(buf, "perf stat -p %d \
    -e cycles                   \
    -e instructions             \
    -e cache-misses             \
    -e cache-references         \
    -e L1-dcache-load-misses    \
    -e L1-dcache-loads          \
    -e LLC-load-misses          \
    -e LLC-loads                \
    -e dTLB-load-misses         \
    -e dTLB-loads               \
    -e node-load-misses         \
    -e node-loads               \
    -e alignment-faults         \
    -e branches                 \
    -e branch-misses            \
    -e branch-loads             \
    -e branch-loads-misses      \
    &",
                getpid());
        // sprintf(buf, "perf stat -p %d -e cycles -e instructions -e cache-misses -e cache-references -e L1-dcache-load-misses -e L1-dcache-loads -e LLC-load-misses -e LLC-loads -e dTLB-load-misses -e dTLB-loads -e node-load-misses -e node-loads -e branches -e branch-misses -e uops_executed.stall_cycles &", getpid());
        auto junk = system(buf);
        for (int i = 0; i < 16; i++) {
            // true_lookup_time = time_lookups(wrap_filter, add_vec, 0, add_step);
            uniform_lookup_time += time_lookups(wrap_filter, find_vec, 0, find_vec->size());
            // true_lookup_time += time_lookups(wrap_filter, add_vec, 0, add_vec->size());
            // uniform_lookup_time += time_lookups(wrap_filter, find_vec, 0, find_vec->size());
        }
        // printf("%zd\n", 500 * add_step);
        printf("%zd\n", 16 * find_vec->size());
        // printf("%zd\n", 16 * add_vec->size());
        // printf("%zd\n", 8 * find_vec->size() + 8 * add_vec->size() );
        // printf("%zd\n", 500 * true_find_step);
        exit(0);
    }

    inline std::string get_int_with_commas(uint64_t x) {
        auto s = std::to_string(x);

        if (s.size() <= 3)
            return s;


        std::string res;
        std::string prefix = s;
        // const size_t s_size = s.size();
        while (prefix.size() > 3) {
            size_t index = prefix.size() - 3;
            std::string temp = prefix.substr(index, 3);
            res = "," + temp + res;

            prefix = prefix.substr(0, index);
        }
        res = prefix + res;
        return res;
    }

    template<class Table>
    void profile_benchmark_cache(Table *wrap_filter, const std::vector<const std::vector<u64> *> *elements) {
        auto add_vec = elements->at(0);
        auto find_vec = elements->at(1);
        // auto delete_vec = elements->at(2);

        auto insertion_time = time_insertions(wrap_filter, add_vec, 0, add_vec->size());

        printf("insertions done\n");
        fflush(stdout);
        ulong uniform_lookup_time = 0;
        // ulong true_lookup_time = 0;
        // size_t true_lookup_time = 0;
        char buf[1024];


        // sprintf(buf, "perf stat -p %d
        sprintf(buf, "perf stat -p %d \
        -e cpu/event=0x2e,umask=0x41,name=LONGEST_LAT_CACHE.MISS/ \
                             & ",
                getpid());

        /* -e cache-misses           \
        -e cache-references       \
        -e L1-dcache-load-misses  \
        -e L1-dcache-loads        \
        -e LLC-load-misses        \
        -e LLC-loads              \
        -e dTLB-load-misses       \
        -e dTLB-loads             \
        -e node-load-misses       \
        -e node-loads             \
        -e branches               \
        -e branch-misses          \
         */
        auto junk = system(buf);
        constexpr size_t reps = 16;
        for (size_t i = 0; i < reps; i++) {
            uniform_lookup_time += time_lookups(wrap_filter, find_vec, 0, find_vec->size());
        }
        // printf("%zd\n", 16 * find_vec->size());
        std::cout << "Number of lookups: \t" << get_int_with_commas(reps * find_vec->size()) << std::endl;
        printf("Number of lookups: %zd\n", reps * find_vec->size());
        exit(0);
    }
}// namespace testSmart

#endif// FILTERS_CON_TESTS_HPP
