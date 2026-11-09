#include "app/options.hpp"
#include "app/pipeline.hpp"
#include <csignal>
#include <exception>
#include <iostream>

namespace {
volatile std::sig_atomic_t interrupted = 0;
void on_signal(int) { interrupted = 1; }
}
int main(int argc, char** argv) {
    try {
        const auto options = image_pipeline::parse_options(argc,argv);
        std::signal(SIGINT,on_signal);
        std::signal(SIGTERM,on_signal);
        image_pipeline::run_pipeline(options,[] { return interrupted != 0; });
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
