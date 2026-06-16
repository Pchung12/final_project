#pragma once
#include "search_types.hpp"
#include "game_history.hpp"

struct Submission1Params {
    bool use_kp_eval = true;
    bool use_eval_mobility = true;
    bool report_partial = false;
    bool use_tt = true;
    bool use_aspiration = true;
    bool use_lmr = true;
    bool use_null_move = true;
    bool use_futility = true;
    bool use_test_eval = true;
    bool use_nnue_eval = false;
    bool include_promotions = true;
    int tt_size_mb = 32;
    int aspiration_window = 50;
    int max_q_ply = 8;
    int nnue_weight = 0;

    static Submission1Params from_map(const ParamMap& m){
        Submission1Params p;
        p.use_kp_eval        = param_bool(m, "UseKPEval", true);
        p.use_eval_mobility  = param_bool(m, "UseEvalMobility", true);
        p.report_partial     = param_bool(m, "ReportPartial", false);
        p.use_tt             = param_bool(m, "UseTT", true);
        p.use_aspiration     = param_bool(m, "UseAspiration", true);
        p.use_lmr            = param_bool(m, "UseLMR", true);
        p.use_null_move      = param_bool(m, "UseNullMove", true);
        p.use_futility       = param_bool(m, "UseFutility", true);
        p.use_test_eval      = param_bool(m, "UseTestEval", true);
        p.use_nnue_eval      = param_bool(m, "UseNNUEEval", false);
        p.include_promotions = param_bool(m, "QuiescencePromotions", true);
        p.tt_size_mb         = param_int(m, "TTSizeMB", 32);
        p.aspiration_window  = param_int(m, "AspirationWindow", 50);
        p.max_q_ply          = param_int(m, "QuiescenceMaxPly", 8);
        p.nnue_weight        = param_int(m, "NNUEWeight", 0);

        if(p.tt_size_mb < 1){
            p.tt_size_mb = 1;
        }
        if(p.aspiration_window < 1){
            p.aspiration_window = 1;
        }
        if(p.max_q_ply < 0){
            p.max_q_ply = 0;
        }
        if(p.nnue_weight < 0){
            p.nnue_weight = 0;
        }
        return p;
    }
};

class Submission1 {
public:
    static SearchResult search(
        State *state,
        int depth,
        GameHistory& history,
        SearchContext& ctx
    );

    static ParamMap default_params();
    static std::vector<ParamDef> param_defs();
};
