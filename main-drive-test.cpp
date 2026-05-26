#include "Tests/smart_tests.hpp"
int THREAD_NUM;

void test_writes(uint64_t argument_1, uint64_t argument_2);

int main(int argc, char* argv[]) {
    if (argc < 4) {
        std::cerr << "Usage: " << argv[0] << " <num_samples> <read_size> <TREAD_NUM>" << std::endl;
        return 1;
    }
    uint64_t argument_1 = std::stoull(argv[1]);
    uint64_t argument_2 = std::stoull(argv[2]);
    THREAD_NUM = std::stoull(argv[3]);
    std::cout << "THREAD_NUM: " << THREAD_NUM << std::endl;
    if (THREAD_NUM > 24 || THREAD_NUM < 1) {
        std::cerr << "THREAD_NUM is " << THREAD_NUM << " which is invalid" << std::endl;
        return 1;
    }
    test_writes(argument_1, argument_2);
    return 0;
}

void appendVectorToBinaryFile(const std::string& filename, const std::vector<uint64_t>& vec) {
    std::ofstream outFile(filename, std::ios::binary | std::ios::app);
    if (!outFile) {
        std::cerr << "Failed to open file for writing: " << filename << std::endl;
        return;
    }
    
    outFile.write(reinterpret_cast<const char*>(vec.data()), vec.size() * sizeof(uint64_t));
    outFile.flush();
    
    int fd = open(filename.c_str(), O_WRONLY | O_APPEND);
    if (fd != -1) {
        fsync(fd);
        close(fd);
    }
    
    outFile.close();
    
    if (!outFile.good()) {
        std::cerr << "Error occurred while writing to file: " << filename << std::endl;
    } else {
        std::cout << "Vector written to file successfully: " << filename << std::endl;
    }
}

void removeFile(const std::string& filename) {
    if (std::remove(filename.c_str()) == 0) {
        std::cout << "File removed successfully: " << filename << std::endl;
    } else {
        std::perror(("Error removing file: " + filename).c_str());
    }
}

void readFirstEntries(const std::string& filename, uint64_t numEntries, std::vector<uint64_t>& data) {
    std::ifstream inFile(filename, std::ios::binary);
    if (!inFile) {
        std::cerr << "Failed to open file for reading: " << filename << std::endl;
        return;
    }
    
    data.resize(numEntries, 0);
    auto start = std::chrono::high_resolution_clock::now();
    
    inFile.read(reinterpret_cast<char*>(data.data()), numEntries * sizeof(uint64_t));
    
    if (!inFile) {
        std::cerr << "Error reading from file. Read only " 
        << inFile.gcount() / sizeof(uint64_t) << " entries." << std::endl;
    } else {
        auto end = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> duration = end - start;
        std::cout << "Successfully read " << numEntries 
        << " entries in " << duration.count() << " seconds." << std::endl;
    }
    
    inFile.close();
}

void test_writes(uint64_t argument_1, uint64_t argument_2) {
    static constexpr double FILL_RATIO = 0.91;
    // The BCF is built to a lower load than the Prefix Filter.
    static constexpr double BCF_FILL_RATIO = 0.91;
    using inc9 = Prefix_Filter<Impala512<>>;
    using BCF = PQF::PQF_8_53;
    using CF8 = cuckoofilter::CuckooFilter<uint64_t, 8>;
    using CF12 = cuckoofilter::CuckooFilter<uint64_t, 12>;
    using CF16 = cuckoofilter::CuckooFilter<uint64_t, 16>;
    using CF32 = cuckoofilter::CuckooFilter<uint64_t, 32>;
    using CF8_N2 = cuckoofilter::CuckooFilterStable<uint64_t, 8>;
    using CF12_Flex = cuckoofilter::CuckooFilterStable<uint64_t, 12>;
    using CF16_N2 = cuckoofilter::CuckooFilterStable<uint64_t, 16>;
    using CF32_N2 = cuckoofilter::CuckooFilterStable<uint64_t, 32>;
    using L_BF8 = bloomfilter::BloomFilter<uint64_t, 8, 0>;
    using L_BF12 = bloomfilter::BloomFilter<uint64_t, 12, 0>;
    using L_BF16 = bloomfilter::BloomFilter<uint64_t, 16, 0>;

    constexpr size_t fp_capacity = ((1ULL << 30)) / testSmart::db_speeder;
    constexpr size_t fp_lookups = fp_capacity;
    constexpr int NUM_FILES = 1;
    std::string filename = "vectors.bin";
    std::vector<uint64_t> fp_v_add;

    uint64_t max_capacity = 1ULL << argument_1;
    uint64_t read_size = static_cast<uint64_t>((1ULL << argument_2) * FILL_RATIO);
    read_size = ((read_size + 1023) / 1024) * 1024;
    // A whole chunk is always hashed, so the chunk has to follow the fill ratio of the filter it
    // feeds, or the filter ends up fuller than intended.
    uint64_t bcf_read_size = static_cast<uint64_t>((1ULL << argument_2) * BCF_FILL_RATIO);
    bcf_read_size = ((bcf_read_size + 1023) / 1024) * 1024;

    if (std::ifstream(filename)) {
        readFirstEntries(filename, fp_capacity, fp_v_add);
    } else {
        for (int i = 0; i < NUM_FILES; i++) {
            testSmart::fill_vec_smart(&fp_v_add, fp_capacity);
            auto start_time = std::chrono::high_resolution_clock::now();
            appendVectorToBinaryFile(filename, fp_v_add);
            auto end_time = std::chrono::high_resolution_clock::now();  
            std::chrono::duration<double> elapsed = end_time - start_time;
            std::cout << std::setw(20) << "Writing File Elapsed Time:" << std::setw(10) << elapsed.count() << " s" << std::endl;
        }
    }
    fp_v_add.clear();

    std::string path = "../scripts/fpp_table.csv";
    std::fstream file(path, std::fstream::in | std::fstream::out | std::fstream::app);
    file << "n =, " << fp_capacity << ", Lookups =, " << fp_lookups << std::endl;
    std::string header = "Filter, Size in bytes, Ratio of yes-queries bits per item (average), optimal bits per item (w.r.t. yes-queries), difference of BPI to optimal BPI, ratio of BPI to optimal BPI";
    file << header << std::endl;
    file.close();
    size_t num_samples = max_capacity * FILL_RATIO;
    num_samples += (4 - (num_samples % 4)); // round up to nearest multiple of 4
    size_t bcf_num_samples = max_capacity * BCF_FILL_RATIO;
    bcf_num_samples += (4 - (bcf_num_samples % 4)); // round up to nearest multiple of 4
    std::cout << "n_entries=" << max_capacity << std::endl;
    // FilterAPI<inc9>::ConstructFromAddCount(num_samples, false);
    std::cout << "\nRunning: inc9" << std::endl;
    for (int i = 0; i < NUM_SAMPLES; i++) {
        testSmart::FPR_test_with_file<inc9>("PrefixFilterBatchStream", filename, true, num_samples, read_size, max_capacity, false);
    }
    std::cout << "Filter=PrefixFilterBatchStream Average time=" << get_average_time() << " Variance=" << get_variance() << std::endl;
    reset_tracker();

    std::cout << "\nRunning: BCF" << std::endl;
    for (int i = 0; i < NUM_SAMPLES; i++) {
        testSmart::FPR_test_with_file<BCF>("BCFBatchStream", filename, true, bcf_num_samples, bcf_read_size, max_capacity, false);
    }
    std::cout << "Filter=BCFBatchStream Average time=" << get_average_time() << " Variance=" << get_variance() << std::endl;
    reset_tracker();
    
}
