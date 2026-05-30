#pragma once
#include "search_types.hpp"
#include "game_history.hpp"

struct QuiescenceParams {
    bool use_kp_eval = true;
    bool use_eval_mobility = true;
    bool report_partial = true;
    bool include_promotions = true;
    int max_q_ply = 8;

    static QuiescenceParams from_map(const ParamMap& m){
        QuiescenceParams p;
        p.use_kp_eval       = param_bool(m, "UseKPEval", true);
        p.use_eval_mobility = param_bool(m, "UseEvalMobility", true);
        p.report_partial    = param_bool(m, "ReportPartial", true);
        p.include_promotions = param_bool(m, "QuiescencePromotions", true);
        p.max_q_ply         = param_int(m, "QuiescenceMaxPly", 8);
        if(p.max_q_ply < 0){
            p.max_q_ply = 0;
        }
        return p;
    }
};

class Quiescence {
public:
    static int eval_ctx(
        State *state,
        GameHistory& history,
        int ply,
        SearchContext& ctx,
        const QuiescenceParams& p,
        int alpha,
        int beta,
        bool count_root = true,
        int q_ply = 0
    );

    static SearchResult search(
        State *state,
        int depth,
        GameHistory& history,
        SearchContext& ctx
    );

    static ParamMap default_params();
    static std::vector<ParamDef> param_defs();
};
