#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <unordered_map>
#include <vector>

#include "state.hpp"
#include "submission.hpp"

namespace {

constexpr int TT_EXACT = 0;
constexpr int TT_LOWER = 1;
constexpr int TT_UPPER = 2;
constexpr int MATE_THRESHOLD = P_MAX - 1000;
constexpr int MAX_SEARCH_PLY = 128;

const Move NO_MOVE(
    Point(std::numeric_limits<size_t>::max(), std::numeric_limits<size_t>::max()),
    Point(std::numeric_limits<size_t>::max(), std::numeric_limits<size_t>::max())
);

const int TUNED_MATERIAL[7] = {
    0, 120, 520, 330, 345, 980, 0
};

struct TTEntry {
    uint64_t key = 0;
    int depth = -1;
    int score = 0;
    int flag = TT_EXACT;
    Move best_move = NO_MOVE;
    bool has_move = false;
    uint16_t age = 0;
};

struct TTProbe {
    bool hit = false;
    bool has_move = false;
    Move move = NO_MOVE;
    int score = 0;
};

std::vector<TTEntry> g_tt;
uint16_t g_tt_age = 1;
std::unordered_map<uint64_t, int> g_root_score;

size_t floor_power_of_two(size_t n){
    size_t p = 1;
    while((p << 1) <= n){
        p <<= 1;
    }
    return p;
}

void configure_tt(const SubmissionParams& p){
    if(!p.use_tt){
        return;
    }

    size_t bytes = static_cast<size_t>(p.tt_size_mb) * 1024ULL * 1024ULL;
    size_t entries = bytes / sizeof(TTEntry);
    if(entries < 1024){
        entries = 1024;
    }
    entries = floor_power_of_two(entries);

    if(g_tt.size() != entries){
        g_tt.assign(entries, TTEntry{});
    }

    g_tt_age++;
    if(g_tt_age == 0){
        g_tt_age = 1;
    }
}

bool same_move(const Move& a, const Move& b){
    return a == b;
}

bool has_move_value(const Move& m){
    return !same_move(m, NO_MOVE);
}

uint64_t move_key(const Move& m){
    return ((static_cast<uint64_t>(m.first.first)  & 0xffffULL) << 48)
         | ((static_cast<uint64_t>(m.first.second) & 0xffffULL) << 32)
         | ((static_cast<uint64_t>(m.second.first) & 0xffffULL) << 16)
         |  (static_cast<uint64_t>(m.second.second) & 0xffffULL);
}

int score_to_tt(int score, int ply){
    if(score > MATE_THRESHOLD){
        return score + ply;
    }
    if(score < -MATE_THRESHOLD){
        return score - ply;
    }
    return score;
}

int score_from_tt(int score, int ply){
    if(score > MATE_THRESHOLD){
        return score - ply;
    }
    if(score < -MATE_THRESHOLD){
        return score + ply;
    }
    return score;
}

TTProbe probe_tt(
    uint64_t key,
    int depth,
    int ply,
    int alpha,
    int beta,
    const SubmissionParams& p
){
    TTProbe probe;
    if(!p.use_tt || g_tt.empty()){
        return probe;
    }

    TTEntry& entry = g_tt[key & (g_tt.size() - 1)];
    if(entry.key != key || entry.depth < 0){
        return probe;
    }

    probe.has_move = entry.has_move;
    probe.move = entry.best_move;

    if(entry.depth < depth){
        return probe;
    }

    int score = score_from_tt(entry.score, ply);
    if(entry.flag == TT_EXACT){
        probe.hit = true;
        probe.score = score;
    }else if(entry.flag == TT_LOWER && score >= beta){
        probe.hit = true;
        probe.score = score;
    }else if(entry.flag == TT_UPPER && score <= alpha){
        probe.hit = true;
        probe.score = score;
    }
    return probe;
}

void store_tt(
    uint64_t key,
    int depth,
    int ply,
    int score,
    int flag,
    const Move& best_move,
    const SubmissionParams& p
){
    if(!p.use_tt || g_tt.empty()){
        return;
    }

    TTEntry& entry = g_tt[key & (g_tt.size() - 1)];
    bool replace = entry.depth < 0
        || entry.key == key
        || entry.age != g_tt_age
        || depth >= entry.depth;
    if(!replace){
        return;
    }

    entry.key = key;
    entry.depth = depth;
    entry.score = score_to_tt(score, ply);
    entry.flag = flag;
    entry.best_move = best_move;
    entry.has_move = has_move_value(best_move);
    entry.age = g_tt_age;
}

int piece_value(int piece){
    if(piece < 0 || piece > 6){
        return 0;
    }
    return TUNED_MATERIAL[piece];
}

bool in_bounds(State *state, int row, int col){
    return row >= 0 && row < state->board_h() && col >= 0 && col < state->board_w();
}

int piece_at_safe(State *state, int player, int row, int col){
    if(!in_bounds(state, row, col)){
        return 0;
    }
    return state->piece_at(player, row, col);
}

bool is_promotion(State *state, const Move& move){
    int fr = static_cast<int>(move.first.first);
    int fc = static_cast<int>(move.first.second);
    int tr = static_cast<int>(move.second.first);
    int piece = piece_at_safe(state, state->player, fr, fc);
    return piece == 1 && (tr == 0 || tr == state->board_h() - 1);
}

bool is_promotion_race(State *state, const Move& move){
    int fr = static_cast<int>(move.first.first);
    int fc = static_cast<int>(move.first.second);
    int tr = static_cast<int>(move.second.first);
    int piece = piece_at_safe(state, state->player, fr, fc);
    if(piece != 1){
        return false;
    }
    if(state->player == 0){
        return tr <= 1;
    }
    return tr >= state->board_h() - 2;
}

int captured_piece(State *state, const Move& move){
    int tr = static_cast<int>(move.second.first);
    int tc = static_cast<int>(move.second.second);
    return piece_at_safe(state, 1 - state->player, tr, tc);
}

bool is_capture(State *state, const Move& move){
    return captured_piece(state, move) != 0;
}

bool is_quiet(State *state, const Move& move){
    return !is_capture(state, move) && !is_promotion(state, move);
}

int capture_gain(State *state, const Move& move){
    int fr = static_cast<int>(move.first.first);
    int fc = static_cast<int>(move.first.second);
    int attacker = piece_at_safe(state, state->player, fr, fc);
    int victim = captured_piece(state, move);
    if(victim == 0){
        return 0;
    }
    if(victim == 6){
        return P_MAX / 2;
    }
    return piece_value(victim) - piece_value(attacker) / 12;
}

int move_static_score(
    State *state,
    const Move& move,
    const TTProbe& tt,
    int ply,
    const std::vector<std::array<Move, 2>>& killers,
    const std::unordered_map<uint64_t, int>& history_scores
){
    if(tt.has_move && same_move(move, tt.move)){
        return 2000000000;
    }

    int score = 0;
    int victim = captured_piece(state, move);
    if(victim){
        int fr = static_cast<int>(move.first.first);
        int fc = static_cast<int>(move.first.second);
        int attacker = piece_at_safe(state, state->player, fr, fc);
        score += 100000000 + piece_value(victim) * 100 - piece_value(attacker);
    }

    if(is_promotion(state, move)){
        score += 90000000;
    }else if(is_promotion_race(state, move)){
        score += 4000000;
    }

    if(ply >= 0 && ply < static_cast<int>(killers.size())){
        if(same_move(move, killers[ply][0])){
            score += 80000000;
        }else if(same_move(move, killers[ply][1])){
            score += 70000000;
        }
    }

    auto it = history_scores.find(move_key(move));
    if(it != history_scores.end()){
        score += std::min(5000000, it->second);
    }

    int tr = static_cast<int>(move.second.first);
    int tc = static_cast<int>(move.second.second);
    int center_r2 = state->board_h() - 1;
    int center_c2 = state->board_w() - 1;
    int dist2 = std::abs(2 * tr - center_r2) + std::abs(2 * tc - center_c2);
    score += 60 - dist2 * 6;
    return score;
}

std::vector<Move> ordered_actions(
    State *state,
    const TTProbe& tt,
    int ply,
    const std::vector<std::array<Move, 2>>& killers,
    const std::unordered_map<uint64_t, int>& history_scores
){
    std::vector<Move> actions = state->legal_actions;
    std::stable_sort(
        actions.begin(),
        actions.end(),
        [&](const Move& a, const Move& b){
            return move_static_score(state, a, tt, ply, killers, history_scores)
                 > move_static_score(state, b, tt, ply, killers, history_scores);
        }
    );
    return actions;
}

bool is_noisy_move(State *state, const Move& move, const SubmissionParams& p){
    if(is_capture(state, move)){
        return true;
    }
    if(!p.include_promotions){
        return false;
    }
    return is_promotion(state, move) || is_promotion_race(state, move);
}

std::vector<Move> ordered_noisy_actions(State *state, const SubmissionParams& p){
    std::vector<Move> actions;
    actions.reserve(state->legal_actions.size());

    for(const auto& move : state->legal_actions){
        if(is_noisy_move(state, move, p)){
            actions.push_back(move);
        }
    }

    std::stable_sort(
        actions.begin(),
        actions.end(),
        [&](const Move& a, const Move& b){
            int score_a = 0;
            int score_b = 0;

            if(is_capture(state, a)){
                score_a += 100000 + capture_gain(state, a) * 10;
            }
            if(is_capture(state, b)){
                score_b += 100000 + capture_gain(state, b) * 10;
            }
            if(is_promotion(state, a)){
                score_a += 60000;
            }
            if(is_promotion(state, b)){
                score_b += 60000;
            }
            if(is_promotion_race(state, a)){
                score_a += 15000;
            }
            if(is_promotion_race(state, b)){
                score_b += 15000;
            }
            return score_a > score_b;
        }
    );
    return actions;
}

class Searcher {
public:
    Searcher(const SubmissionParams& params, SearchContext& context)
        : p(params), ctx(context) {
        killers.assign(MAX_SEARCH_PLY, {NO_MOVE, NO_MOVE});
    }

    SearchResult run(State *state, int depth, GameHistory& history){
        SearchResult result;
        result.depth = depth;

        if(state->legal_actions.empty() && state->game_state == UNKNOWN){
            state->get_legal_actions();
        }

        if(state->legal_actions.empty()){
            result.score = evaluate(state, history);
            result.nodes = ctx.nodes;
            result.seldepth = ctx.seldepth;
            return result;
        }

        if(state->game_state == WIN){
            result.best_move = state->legal_actions[0];
            result.score = P_MAX;
            result.nodes = ctx.nodes;
            result.seldepth = ctx.seldepth;
            result.pv = {result.best_move};
            return result;
        }

        int alpha = M_MAX;
        int beta = P_MAX;
        bool used_aspiration = false;
        uint64_t root_hash = state->hash();

        if(p.use_aspiration && depth >= 2){
            auto it = g_root_score.find(root_hash);
            if(it != g_root_score.end()){
                int window = p.aspiration_window;
                alpha = std::max(M_MAX, it->second - window);
                beta = std::min(P_MAX, it->second + window);
                used_aspiration = true;
            }
        }

        result = root_search(state, depth, history, alpha, beta);

        if(used_aspiration && !ctx.stop
           && (result.score <= alpha || result.score >= beta)){
            result = root_search(state, depth, history, M_MAX, P_MAX);
        }

        g_root_score[root_hash] = result.score;
        result.nodes = ctx.nodes;
        result.seldepth = ctx.seldepth;
        if(result.pv.empty() && has_move_value(result.best_move)){
            result.pv = {result.best_move};
        }
        return result;
    }

private:
    SubmissionParams p;
    SearchContext& ctx;
    std::vector<std::array<Move, 2>> killers;
    std::unordered_map<uint64_t, int> history_scores;

    int evaluate(State *state, GameHistory& history){
        return state->evaluate(p.use_kp_eval, p.use_eval_mobility, &history);
    }

    void update_quiet_cutoff(const Move& move, int depth, int ply){
        if(ply >= 0 && ply < static_cast<int>(killers.size())){
            if(!same_move(move, killers[ply][0])){
                killers[ply][1] = killers[ply][0];
                killers[ply][0] = move;
            }
        }

        int bonus = depth * depth + 2 * depth + 1;
        int& hist = history_scores[move_key(move)];
        hist = std::min(5000000, hist + bonus * 128);
    }

    int search_child(
        State *child,
        int depth,
        GameHistory& history,
        int ply,
        int alpha,
        int beta,
        bool same_player
    ){
        int raw = same_player
            ? pvs(child, depth, history, ply, alpha, beta)
            : pvs(child, depth, history, ply, -beta, -alpha);
        return same_player ? raw : -raw;
    }

    int quiescence_child(
        State *child,
        GameHistory& history,
        int ply,
        int q_ply,
        int alpha,
        int beta,
        bool same_player
    ){
        int raw = same_player
            ? quiescence(child, history, ply, q_ply, alpha, beta)
            : quiescence(child, history, ply, q_ply, -beta, -alpha);
        return same_player ? raw : -raw;
    }

    int quiescence(
        State *state,
        GameHistory& history,
        int ply,
        int q_ply,
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

        uint64_t key = state->hash();
        history.push(key);

        int stand_pat = evaluate(state, history);
        if(stand_pat >= beta){
            history.pop(key);
            return stand_pat;
        }
        if(stand_pat > alpha){
            alpha = stand_pat;
        }
        if(q_ply >= p.max_q_ply){
            history.pop(key);
            return alpha;
        }

        auto actions = ordered_noisy_actions(state, p);
        for(const Move& action : actions){
            if(ctx.stop){
                break;
            }

            int victim = captured_piece(state, action);
            int margin = capture_gain(state, action);
            if(is_promotion(state, action)){
                margin += piece_value(5) - piece_value(1);
            }else if(is_promotion_race(state, action)){
                margin += 220;
            }
            if(victim != 6 && stand_pat + margin + 180 < alpha){
                continue;
            }

            State* next = state->next_state(action);
            bool same = next->same_player_as_parent();
            int score = quiescence_child(
                next, history, ply + 1, q_ply + 1, alpha, beta, same
            );
            delete next;

            if(score >= beta){
                history.pop(key);
                return score;
            }
            if(score > alpha){
                alpha = score;
            }
        }

        history.pop(key);
        return alpha;
    }

    int pvs(
        State *state,
        int depth,
        GameHistory& history,
        int ply,
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
            return quiescence(state, history, ply, 0, alpha, beta);
        }

        uint64_t key = state->hash();
        int alpha_orig = alpha;
        int beta_orig = beta;
        TTProbe tt = probe_tt(key, depth, ply, alpha, beta, p);
        if(tt.hit){
            return tt.score;
        }

        history.push(key);

        auto actions = ordered_actions(
            state, tt, ply, killers, history_scores
        );
        if(actions.empty()){
            int score = evaluate(state, history);
            history.pop(key);
            return score;
        }

        int best_score = M_MAX;
        Move best_move = NO_MOVE;
        bool first_child = true;

        for(const Move& action : actions){
            State* next = state->next_state(action);
            bool same = next->same_player_as_parent();
            int score;

            if(first_child){
                score = search_child(
                    next, depth - 1, history, ply + 1, alpha, beta, same
                );
                first_child = false;
            }else{
                score = search_child(
                    next, depth - 1, history, ply + 1, alpha, alpha + 1, same
                );
                if(score > alpha && score < beta && !ctx.stop){
                    score = search_child(
                        next, depth - 1, history, ply + 1, alpha, beta, same
                    );
                }
            }

            delete next;

            if(score > best_score){
                best_score = score;
                best_move = action;
            }
            if(score > alpha){
                alpha = score;
            }
            if(alpha >= beta || ctx.stop){
                if(is_quiet(state, action)){
                    update_quiet_cutoff(action, depth, ply);
                }
                break;
            }
        }

        history.pop(key);

        int flag = TT_EXACT;
        if(best_score <= alpha_orig){
            flag = TT_UPPER;
        }else if(best_score >= beta_orig){
            flag = TT_LOWER;
        }
        if(!ctx.stop){
            store_tt(key, depth, ply, best_score, flag, best_move, p);
        }
        return best_score;
    }

    SearchResult root_search(
        State *state,
        int depth,
        GameHistory& history,
        int alpha,
        int beta
    ){
        SearchResult result;
        result.depth = depth;

        TTProbe tt = probe_tt(state->hash(), depth, 0, alpha, beta, p);
        auto actions = ordered_actions(
            state, tt, 0, killers, history_scores
        );
        int total_moves = static_cast<int>(actions.size());
        if(total_moves == 0){
            result.score = evaluate(state, history);
            result.nodes = ctx.nodes;
            result.seldepth = ctx.seldepth;
            return result;
        }

        int alpha_orig = alpha;
        int beta_orig = beta;
        int best_score = M_MAX;
        Move best_move = actions[0];
        bool first_child = true;

        for(int move_index = 0; move_index < total_moves; move_index++){
            const Move& action = actions[move_index];
            State* next = state->next_state(action);
            bool same = next->same_player_as_parent();
            int score;

            if(first_child){
                score = search_child(
                    next, depth - 1, history, 1, alpha, beta, same
                );
                first_child = false;
            }else{
                score = search_child(
                    next, depth - 1, history, 1, alpha, alpha + 1, same
                );
                if(score > alpha && score < beta && !ctx.stop){
                    score = search_child(
                        next, depth - 1, history, 1, alpha, beta, same
                    );
                }
            }

            delete next;

            if(score > best_score){
                best_score = score;
                best_move = action;
                result.best_move = best_move;

                if(p.report_partial && ctx.on_root_update){
                    ctx.on_root_update({
                        result.best_move,
                        best_score,
                        depth,
                        move_index + 1,
                        total_moves
                    });
                }
            }

            if(score > alpha){
                alpha = score;
            }
            if(alpha >= beta || ctx.stop){
                if(is_quiet(state, action)){
                    update_quiet_cutoff(action, depth, 0);
                }
                break;
            }
        }

        int flag = TT_EXACT;
        if(best_score <= alpha_orig){
            flag = TT_UPPER;
        }else if(best_score >= beta_orig){
            flag = TT_LOWER;
        }
        if(!ctx.stop){
            store_tt(state->hash(), depth, 0, best_score, flag, best_move, p);
        }

        result.best_move = best_move;
        result.score = best_score;
        result.nodes = ctx.nodes;
        result.seldepth = ctx.seldepth;
        result.pv = {best_move};
        return result;
    }
};

}  // namespace

SearchResult Submission::search(
    State *state,
    int depth,
    GameHistory& history,
    SearchContext& ctx
){
    ctx.reset();
    SubmissionParams p = SubmissionParams::from_map(ctx.params);
    configure_tt(p);

    Searcher searcher(p, ctx);
    return searcher.run(state, depth, history);
}

ParamMap Submission::default_params(){
    return {
        {"UseKPEval", "true"},
        {"UseEvalMobility", "true"},
        {"ReportPartial", "true"},
        {"UseTT", "true"},
        {"TTSizeMB", "32"},
        {"UseAspiration", "true"},
        {"AspirationWindow", "50"},
        {"QuiescenceMaxPly", "10"},
        {"QuiescencePromotions", "true"},
    };
}

std::vector<ParamDef> Submission::param_defs(){
    return {
        {"UseKPEval", ParamDef::CHECK, "true"},
        {"UseEvalMobility", ParamDef::CHECK, "true"},
        {"ReportPartial", ParamDef::CHECK, "true"},
        {"UseTT", ParamDef::CHECK, "true"},
        {"TTSizeMB", ParamDef::SPIN, "32", 1, 256},
        {"UseAspiration", ParamDef::CHECK, "true"},
        {"AspirationWindow", ParamDef::SPIN, "50", 1, 1000},
        {"QuiescenceMaxPly", ParamDef::SPIN, "10", 0, 24},
        {"QuiescencePromotions", ParamDef::CHECK, "true"},
    };
}
