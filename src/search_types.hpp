#pragma once
#include "base_state.hpp"
#include "search_params.hpp"
#include <vector>
#include <cstdint>
#include <functional>
#include <chrono>

class State;

struct RootUpdate {
    Move best_move;
    int score;
    int depth;
    int move_number;
    int total_moves;
};

struct SearchContext {
    uint64_t nodes = 0;
    int seldepth = 0;
    bool stop = false;
    bool use_deadline = false;
    std::chrono::steady_clock::time_point deadline;
    ParamMap params;
    std::function<void(const RootUpdate&)> on_root_update;

    void reset(){
        nodes = 0;
        seldepth = 0;
    }

    bool should_stop(){
        if(stop){
            return true;
        }
        if(use_deadline && std::chrono::steady_clock::now() >= deadline){
            stop = true;
            return true;
        }
        return false;
    }
};

struct SearchResult {
    Move best_move;
    int score = 0;
    int depth = 0;
    int seldepth = 0;
    uint64_t nodes = 0;
    double time_ms = 0;
    std::vector<Move> pv;
};
