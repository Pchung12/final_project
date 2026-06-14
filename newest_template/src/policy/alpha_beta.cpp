#include <utility>
#include "state.hpp"
#include "alpha_beta.hpp"


/*============================================================
 * AlphaBeta -- eval_ctx
 *
 * Negamax with alpha-beta pruning. Caller manages memory.
 *============================================================*/
int AlphaBeta::eval_ctx(
    State *state,
    int depth,
    GameHistory& history,
    int ply,
    SearchContext& ctx,
    const ABParams& p,
    // Carry alpha-beta bounds through the recursive search.
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

    /* === Lazy move generation (sets game_state) === */
    if(state->legal_actions.empty() && state->game_state == UNKNOWN){
        state->get_legal_actions();
    }

    /* === Terminal / leaf checks === */

    // Return the score for a winning terminal state and prefer faster wins.
    if(state->game_state == WIN){
        return P_MAX - ply;
    }

    if(state->game_state == DRAW){
        return 0;
    }

    /* === Repetition check (game-specific) === */
    int rep_score;
    if(state->check_repetition(history, rep_score)){
        return rep_score;
    }
    history.push(state->hash());

    if(depth <= 0){
        int score = state->evaluate(
            p.use_kp_eval, p.use_eval_mobility, &history
        );
        history.pop(state->hash());
        return score;
    }

    /* === Alpha-beta negamax loop === */
    int best_score = M_MAX;

    for(auto& action : state->legal_actions){
        // Create the child state after applying this action.
        State* next = state->next_state(action);

        bool same = next->same_player_as_parent();

        // Search the child with a flipped window when the turn changes.
        int raw = same
            ? eval_ctx(next, depth - 1, history, ply + 1, ctx, p, alpha, beta)
            : eval_ctx(next, depth - 1, history, ply + 1, ctx, p, -beta, -alpha);

        // Convert the child score back to the current player's perspective.
        int score = same ? raw : -raw;

        delete next;

        // Keep the best child score found at this node.
        if(score > best_score){
            best_score = score;
        }

        // Update alpha with the best score found for this node.
        if(best_score > alpha){
            alpha = best_score;
        }

        // Stop searching this node when alpha proves beta cannot improve.
        if(alpha >= beta){
            break;
        }
    }

    history.pop(state->hash());
    return best_score;
}


/*============================================================
 * AlphaBeta -- search
 *
 * Iterate legal moves, call eval_ctx, return SearchResult.
 *============================================================*/
SearchResult AlphaBeta::search(
    State *state,
    int depth,
    GameHistory& history,
    SearchContext& ctx
){
    ctx.reset();
    ABParams p = ABParams::from_map(ctx.params);
    SearchResult result;
    result.depth = depth;

    if(!state->legal_actions.size()){
        state->get_legal_actions();
    }

    int best_score = M_MAX - 10;
    // Initialize the root alpha-beta window before trying candidate moves.
    int alpha = M_MAX;
    int beta = P_MAX;
    int move_index = 0;
    int total_moves = (int)state->legal_actions.size();

    if(total_moves == 0){
        result.best_move = Move();
        result.score = state->evaluate(
            p.use_kp_eval, p.use_eval_mobility, &history
        );
        result.nodes = ctx.nodes;
        result.seldepth = ctx.seldepth;
        return result;
    }

    for(auto& action : state->legal_actions){
        // Create the root child before evaluating this candidate move.
        State* next = state->next_state(action);
        bool same = next->same_player_as_parent();

        // Search the root child using the current alpha-beta window.
        int raw = same
            ? eval_ctx(next, depth - 1, history, 1, ctx, p, alpha, beta)
            : eval_ctx(next, depth - 1, history, 1, ctx, p, -beta, -alpha);
        int score = same ? raw : -raw;
        delete next;

        // Keep this root move if it is the best move found so far.
        if(score > best_score){
            best_score = score;
            result.best_move = action;

            if(p.report_partial && ctx.on_root_update){
               ctx.on_root_update({result.best_move, best_score, depth, move_index + 1, total_moves});
            }
        }

        // Raise root alpha after each searched move so later moves can be pruned.
        if(best_score > alpha){
            alpha = best_score;
        }
        move_index++;
    }

    // Store the final root search result before returning it.
    result.score = best_score;
    result.nodes = ctx.nodes;
    result.seldepth = ctx.seldepth;
    result.pv = {result.best_move};

    return result;
}


/*============================================================
 * AlphaBeta -- default_params / param_defs
 *============================================================*/
ParamMap AlphaBeta::default_params(){
    return {
        {"UseKPEval", "true"},
        {"UseEvalMobility", "true"},
        {"ReportPartial", "true"},
    };
}

std::vector<ParamDef> AlphaBeta::param_defs(){
    return {
        {"UseKPEval", ParamDef::CHECK, "true"},
        {"UseEvalMobility", ParamDef::CHECK, "true"},
        {"ReportPartial", ParamDef::CHECK, "true"},
    };
}
