#pragma once
#include "search_types.hpp"
#include "game_history.hpp"

struct SubmissionParams {
    bool use_kp_eval = true;
    bool use_eval_mobility = true;
    bool report_partial = true;
    bool use_tt = true;
    bool use_aspiration = true;
    bool include_promotions = true;
    int tt_size_mb = 32;
    int aspiration_window = 50;
    int max_q_ply = 10;

    static SubmissionParams from_map(const ParamMap& m){
        SubmissionParams p;
        p.use_kp_eval        = param_bool(m, "UseKPEval", true);
        p.use_eval_mobility  = param_bool(m, "UseEvalMobility", true);
        p.report_partial     = param_bool(m, "ReportPartial", true);
        p.use_tt             = param_bool(m, "UseTT", true);
        p.use_aspiration     = param_bool(m, "UseAspiration", true);
        p.include_promotions = param_bool(m, "QuiescencePromotions", true);
        p.tt_size_mb         = param_int(m, "TTSizeMB", 32);
        p.aspiration_window  = param_int(m, "AspirationWindow", 50);
        p.max_q_ply          = param_int(m, "QuiescenceMaxPly", 10);

        if(p.tt_size_mb < 1){
            p.tt_size_mb = 1;
        }
        if(p.aspiration_window < 1){
            p.aspiration_window = 1;
        }
        if(p.max_q_ply < 0){
            p.max_q_ply = 0;
        }
        return p;
    }
};

class Submission {
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
