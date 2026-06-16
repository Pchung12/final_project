#pragma once

#include "search_types.hpp"
#include "game_history.hpp"

struct TestParams {
    bool use_kp_eval = true;
    bool use_eval_mobility = true;
    bool use_quiescence = true;
    bool use_move_ordering = true;
    bool use_tt = true;
    bool use_null_move = true;
    bool use_killer_moves = true;
    bool use_lmr = true;
    bool report_partial = true;

    int hash_mb = 24;
    int quiescence_max_depth = 16;
    int killer_slots = 2;
    int null_move_r = 2;
    int lmr_full_depth = 3;
    int lmr_depth_limit = 3;

    static TestParams from_map(const ParamMap& m);
};

class Test {
public:
    static SearchResult search(
        State* state,
        int depth,
        GameHistory& history,
        SearchContext& ctx
    );

    static ParamMap default_params();
    static std::vector<ParamDef> param_defs();
};
