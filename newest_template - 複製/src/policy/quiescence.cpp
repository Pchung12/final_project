#include <algorithm>
#include <vector>
#include "state.hpp"
#include "quiescence.hpp"

namespace {

int piece_value(int piece){
    if(piece < 0 || piece > 6){
        return 0;
    }
    return PIECE_VALUES[piece];
}

bool is_promotion(State *state, const Move& move){
    int player = state->player;
    int fr = (int)move.first.first;
    int fc = (int)move.first.second;
    int tr = (int)move.second.first;
    int piece = state->piece_at(player, fr, fc);
    return piece == 1 && (tr == 0 || tr == state->board_h() - 1);
}

bool is_noisy_move(State *state, const Move& move, const QuiescenceParams& p){
    int opp = 1 - state->player;
    int tr = (int)move.second.first;
    int tc = (int)move.second.second;

    if(state->piece_at(opp, tr, tc) != 0){
        return true;
    }
    return p.include_promotions && is_promotion(state, move);
}

int move_order_score(State *state, const Move& move, const QuiescenceParams& p){
    int player = state->player;
    int opp = 1 - player;
    int fr = (int)move.first.first;
    int fc = (int)move.first.second;
    int tr = (int)move.second.first;
    int tc = (int)move.second.second;

    int attacker = state->piece_at(player, fr, fc);
    int victim = state->piece_at(opp, tr, tc);
    int score = 0;

    if(victim){
        score += 10000 + piece_value(victim) * 16 - piece_value(attacker);
    }
    if(p.include_promotions && is_promotion(state, move)){
        score += 800;
    }

    return score;
}

std::vector<Move> ordered_noisy_actions(State *state, const QuiescenceParams& p){
    std::vector<Move> actions;
    actions.reserve(state->legal_actions.size());

    for(const auto& action : state->legal_actions){
        if(is_noisy_move(state, action, p)){
            actions.push_back(action);
        }
    }

    std::stable_sort(
        actions.begin(),
        actions.end(),
        [state, &p](const Move& a, const Move& b){
            return move_order_score(state, a, p) > move_order_score(state, b, p);
        }
    );
    return actions;
}

std::vector<Move> ordered_actions(State *state, const QuiescenceParams& p){
    std::vector<Move> actions = state->legal_actions;
    std::stable_sort(
        actions.begin(),
        actions.end(),
        [state, &p](const Move& a, const Move& b){
            return move_order_score(state, a, p) > move_order_score(state, b, p);
        }
    );
    return actions;
}

int search_quiescence_child(
    State *child,
    GameHistory& history,
    int ply,
    SearchContext& ctx,
    const QuiescenceParams& p,
    int alpha,
    int beta,
    bool same_player,
    int q_ply
){
    int raw = same_player
        ? Quiescence::eval_ctx(child, history, ply, ctx, p, alpha, beta, true, q_ply)
        : Quiescence::eval_ctx(child, history, ply, ctx, p, -beta, -alpha, true, q_ply);
    return same_player ? raw : -raw;
}

int eval_search_ctx(
    State *state,
    int depth,
    GameHistory& history,
    int ply,
    SearchContext& ctx,
    const QuiescenceParams& p,
    int alpha,
    int beta
);

int search_full_child(
    State *child,
    int depth,
    GameHistory& history,
    int ply,
    SearchContext& ctx,
    const QuiescenceParams& p,
    int alpha,
    int beta,
    bool same_player
){
    int raw = same_player
        ? eval_search_ctx(child, depth, history, ply, ctx, p, alpha, beta)
        : eval_search_ctx(child, depth, history, ply, ctx, p, -beta, -alpha);
    return same_player ? raw : -raw;
}

int eval_search_ctx(
    State *state,
    int depth,
    GameHistory& history,
    int ply,
    SearchContext& ctx,
    const QuiescenceParams& p,
    int alpha,
    int beta
){
    ctx.nodes++;
    if(ply > ctx.seldepth){
        ctx.seldepth = ply;
    }
    if(ctx.stop){
        return 0;
    }

    if(state->legal_actions.empty() && state->game_state == UNKNOWN){
        state->get_legal_actions();
    }

    if(state->game_state == WIN){
        return P_MAX - ply;
    }
    if(state->game_state == DRAW){
        return 0;
    }

    int rep_score;
    if(state->check_repetition(history, rep_score)){
        return rep_score;
    }

    if(depth <= 0){
        return Quiescence::eval_ctx(
            state, history, ply, ctx, p, alpha, beta, false, 0
        );
    }

    history.push(state->hash());

    int best_score = M_MAX;
    bool first_child = true;
    auto actions = ordered_actions(state, p);

    for(auto& action : actions){
        State* next = state->next_state(action);
        bool same = next->same_player_as_parent();
        int score;

        if(first_child){
            score = search_full_child(
                next, depth - 1, history, ply + 1, ctx, p, alpha, beta, same
            );
            first_child = false;
        }else{
            score = search_full_child(
                next, depth - 1, history, ply + 1, ctx, p, alpha, alpha + 1, same
            );
            if(score > alpha && score < beta && !ctx.stop){
                score = search_full_child(
                    next, depth - 1, history, ply + 1, ctx, p, alpha, beta, same
                );
            }
        }

        delete next;

        if(score > best_score){
            best_score = score;
        }
        if(best_score > alpha){
            alpha = best_score;
        }
        if(alpha >= beta || ctx.stop){
            break;
        }
    }

    history.pop(state->hash());
    return best_score;
}

}

int Quiescence::eval_ctx(
    State *state,
    GameHistory& history,
    int ply,
    SearchContext& ctx,
    const QuiescenceParams& p,
    int alpha,
    int beta,
    bool count_root,
    int q_ply
){
    if(count_root){
        ctx.nodes++;
    }
    if(ply > ctx.seldepth){
        ctx.seldepth = ply;
    }
    if(ctx.stop){
        return 0;
    }

    if(state->legal_actions.empty() && state->game_state == UNKNOWN){
        state->get_legal_actions();
    }

    if(state->game_state == WIN){
        return P_MAX - ply;
    }
    if(state->game_state == DRAW){
        return 0;
    }

    int rep_score;
    if(state->check_repetition(history, rep_score)){
        return rep_score;
    }
    history.push(state->hash());

    int stand_pat = state->evaluate(
        p.use_kp_eval, p.use_eval_mobility, &history
    );

    if(stand_pat >= beta){
        history.pop(state->hash());
        return beta;
    }
    if(stand_pat > alpha){
        alpha = stand_pat;
    }

    if(q_ply >= p.max_q_ply){
        history.pop(state->hash());
        return alpha;
    }

    auto actions = ordered_noisy_actions(state, p);
    for(auto& action : actions){
        State* next = state->next_state(action);
        bool same = next->same_player_as_parent();

        int score = search_quiescence_child(
            next, history, ply + 1, ctx, p, alpha, beta, same, q_ply + 1
        );
        delete next;

        if(score >= beta){
            history.pop(state->hash());
            return beta;
        }
        if(score > alpha){
            alpha = score;
        }
        if(ctx.stop){
            break;
        }
    }

    history.pop(state->hash());
    return alpha;
}

SearchResult Quiescence::search(
    State *state,
    int depth,
    GameHistory& history,
    SearchContext& ctx
){
    ctx.reset();
    QuiescenceParams p = QuiescenceParams::from_map(ctx.params);
    SearchResult result;
    result.depth = depth;

    if(state->legal_actions.empty()){
        state->get_legal_actions();
    }

    auto actions = ordered_actions(state, p);
    int total_moves = (int)actions.size();
    if(total_moves == 0){
        result.best_move = Move();
        result.score = state->evaluate(
            p.use_kp_eval, p.use_eval_mobility, &history
        );
        result.nodes = ctx.nodes;
        result.seldepth = ctx.seldepth;
        return result;
    }

    int best_score = M_MAX - 10;
    int alpha = M_MAX;
    int beta = P_MAX;
    bool first_child = true;

    for(int move_index = 0; move_index < total_moves; move_index++){
        const Move& action = actions[move_index];
        State* next = state->next_state(action);
        bool same = next->same_player_as_parent();
        int score;

        if(first_child){
            score = search_full_child(
                next, depth - 1, history, 1, ctx, p, alpha, beta, same
            );
            first_child = false;
        }else{
            score = search_full_child(
                next, depth - 1, history, 1, ctx, p, alpha, alpha + 1, same
            );
            if(score > alpha && score < beta && !ctx.stop){
                score = search_full_child(
                    next, depth - 1, history, 1, ctx, p, alpha, beta, same
                );
            }
        }

        delete next;

        if(score > best_score){
            best_score = score;
            result.best_move = action;

            if(p.report_partial && ctx.on_root_update){
                ctx.on_root_update({
                    result.best_move, best_score, depth, move_index + 1, total_moves
                });
            }
        }

        if(best_score > alpha){
            alpha = best_score;
        }
        if(ctx.stop){
            break;
        }
    }

    result.score = best_score;
    result.nodes = ctx.nodes;
    result.seldepth = ctx.seldepth;
    result.pv = {result.best_move};
    return result;
}

ParamMap Quiescence::default_params(){
    return {
        {"UseKPEval", "true"},
        {"UseEvalMobility", "true"},
        {"ReportPartial", "true"},
        {"QuiescenceMaxPly", "8"},
        {"QuiescencePromotions", "true"},
    };
}

std::vector<ParamDef> Quiescence::param_defs(){
    return {
        {"UseKPEval", ParamDef::CHECK, "true"},
        {"UseEvalMobility", ParamDef::CHECK, "true"},
        {"ReportPartial", ParamDef::CHECK, "true"},
        {"QuiescenceMaxPly", ParamDef::SPIN, "8", 0, 16},
        {"QuiescencePromotions", ParamDef::CHECK, "true"},
    };
}
