static constexpr uint64_t NUM_SAMPLES = 5;
static double ns_per_entry[NUM_SAMPLES];
static uint64_t cur_iter = 0;

static inline void reset_tracker() {
    cur_iter = 0;
}

static inline void track_time(double ns) {
    ns_per_entry[cur_iter++] = ns;
}

static inline double get_average_time() {
    if (cur_iter != NUM_SAMPLES) {
        std::cout << "Not enough samples to get average time" << std::endl;
        exit(1);
    }
    double sum = 0;
    for (uint64_t i = 0; i < NUM_SAMPLES; i++) {
        sum += ns_per_entry[i];
    }
    return sum / NUM_SAMPLES;
}

static inline double get_variance() {
    if (cur_iter != NUM_SAMPLES) {
        std::cout << "Not enough samples to get variance" << std::endl;
        exit(1);
    }
    double sum = 0;
    double sum_squared = 0;
    for (uint64_t i = 0; i < NUM_SAMPLES; i++) {
        sum += ns_per_entry[i];
        sum_squared += ns_per_entry[i] * ns_per_entry[i];
    }
    return sum_squared / NUM_SAMPLES - (sum / NUM_SAMPLES) * (sum / NUM_SAMPLES);
}