#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <unordered_map>
#include <vector>


#include "state.hpp"
#include "submission1.hpp"


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

void configure_tt(const Submission1Params& p){
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
    const Submission1Params& p
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
    const Submission1Params& p
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
    struct ScoredMove {
        Move move;
        int score;
    };

    std::vector<ScoredMove> scored;
    scored.reserve(state->legal_actions.size());
    for(const auto& move : state->legal_actions){
        scored.push_back({
            move,
            move_static_score(state, move, tt, ply, killers, history_scores)
        });
    }

    std::stable_sort(
        scored.begin(),
        scored.end(),
        [](const ScoredMove& a, const ScoredMove& b){
            return a.score > b.score;
        }
    );

    std::vector<Move> actions;
    actions.reserve(scored.size());
    for(const auto& item : scored){
        actions.push_back(item.move);
    }
    return actions;
}


bool is_noisy_move(State *state, const Move& move, const Submission1Params& p){
    if(is_capture(state, move)){
        return true;
    }
    if(!p.include_promotions){
        return false;
    }
    return is_promotion(state, move) || is_promotion_race(state, move);
}


std::vector<Move> ordered_noisy_actions(State *state, const Submission1Params& p){
    struct ScoredMove {
        Move move;
        int score;
    };

    std::vector<ScoredMove> scored;
    scored.reserve(state->legal_actions.size());


    for(const auto& move : state->legal_actions){
        if(is_noisy_move(state, move, p)){
            int score = 0;
            if(is_capture(state, move)){
                score += 100000 + capture_gain(state, move) * 10;
            }
            if(is_promotion(state, move)){
                score += 60000;
            }
            if(is_promotion_race(state, move)){
                score += 15000;
            }
            scored.push_back({move, score});
        }
    }


    std::stable_sort(
        scored.begin(),
        scored.end(),
        [](const ScoredMove& a, const ScoredMove& b){
            return a.score > b.score;
        }
    );

    std::vector<Move> actions;
    actions.reserve(scored.size());
    for(const auto& item : scored){
        actions.push_back(item.move);
    }
    return actions;
}


int pawn_promotion_distance(State *state, int player, int row){
    return player == 0 ? row : (state->board_h() - 1 - row);
}


bool has_non_pawn_material(State *state, int player){
    for(int r = 0; r < state->board_h(); r++){
        for(int c = 0; c < state->board_w(); c++){
            int piece = piece_at_safe(state, player, r, c);
            if(piece != 0 && piece != 1 && piece != 6){
                return true;
            }
        }
    }
    return false;
}


bool has_promotion_race_pawn(State *state, int player){
    for(int r = 0; r < state->board_h(); r++){
        for(int c = 0; c < state->board_w(); c++){
            if(piece_at_safe(state, player, r, c) == 1
               && pawn_promotion_distance(state, player, r) <= 1){
                return true;
            }
        }
    }
    return false;
}


bool is_passed_pawn(State *state, int player, int row, int col){
    int opponent = 1 - player;
    for(int dc = -1; dc <= 1; dc++){
        int c = col + dc;
        if(c < 0 || c >= state->board_w()){
            continue;
        }
        if(player == 0){
            for(int r = row - 1; r >= 0; r--){
                if(piece_at_safe(state, opponent, r, c) == 1){
                    return false;
                }
            }
        }else{
            for(int r = row + 1; r < state->board_h(); r++){
                if(piece_at_safe(state, opponent, r, c) == 1){
                    return false;
                }
            }
        }
    }
    return true;
}


struct CheckStatus {
    bool in_check = false;
    bool mated = false;
    int safe_evasions = 0;
};


bool opponent_can_capture_king(State *state){
    State probe(state->board, 1 - state->player);
    probe.step = state->step;
    probe.get_legal_actions();
    return probe.game_state == WIN;
}


CheckStatus inspect_check_status(State *state){
    CheckStatus status;
    if(state->legal_actions.empty() && state->game_state == UNKNOWN){
        state->get_legal_actions();
    }

    status.in_check = opponent_can_capture_king(state);
    if(!status.in_check){
        return status;
    }

    for(const Move& action : state->legal_actions){
        State* next = state->next_state(action);
        if(next->legal_actions.empty() && next->game_state == UNKNOWN){
            next->get_legal_actions();
        }
        if(next->game_state != WIN){
            status.safe_evasions++;
        }
        delete next;
    }

    status.mated = status.safe_evasions == 0;
    return status;
}


bool move_gives_check(State *state, const Move& action){
    State* next = state->next_state(action);
    bool gives = opponent_can_capture_king(next);
    delete next;
    return gives;
}


int player_eval_bonus(State *state, int player){
    int bonus = 0;
    for(int r = 0; r < state->board_h(); r++){
        for(int c = 0; c < state->board_w(); c++){
            int piece = piece_at_safe(state, player, r, c);
            if(piece == 0){
                continue;
            }


            int center_r2 = state->board_h() - 1;
            int center_c2 = state->board_w() - 1;
            int dist2 = std::abs(2 * r - center_r2)
                      + std::abs(2 * c - center_c2);
            if(piece != 1 && piece != 6){
                bonus += std::max(0, 18 - dist2 * 2);
            }


            if(piece == 1){
                int dist = pawn_promotion_distance(state, player, r);
                bonus += (state->board_h() - 1 - dist) * 12;
                if(dist <= 1){
                    bonus += 95;
                }
                if(is_passed_pawn(state, player, r, c)){
                    bonus += 35 + (state->board_h() - 1 - dist) * 10;
                }
            }
        }
    }
    return bonus;
}


constexpr int NNUE_HIDDEN = 12;
const int NNUE_BIAS[NNUE_HIDDEN] = {
    8, 12, 6, 4, 10, 8, 12, 6, 6, 5, 4, 8
};
const int NNUE_OUT[NNUE_HIDDEN] = {
    13, 9, 11, 10, 7, 9, 8, 5, 5, 6, 7, 4
};
const int NNUE_TROPISM[7] = {
    0, 1, 4, 4, 3, 6, 0
};


int crelu(int value){
    return std::max(0, std::min(255, value));
}


bool find_king(State *state, int player, int& row, int& col){
    for(int r = 0; r < state->board_h(); r++){
        for(int c = 0; c < state->board_w(); c++){
            if(piece_at_safe(state, player, r, c) == 6){
                row = r;
                col = c;
                return true;
            }
        }
    }
    row = -1;
    col = -1;
    return false;
}


int center_bonus(State *state, int row, int col, int max_bonus){
    int center_r2 = state->board_h() - 1;
    int center_c2 = state->board_w() - 1;
    int dist2 = std::abs(2 * row - center_r2)
              + std::abs(2 * col - center_c2);
    return std::max(0, max_bonus - dist2);
}


int sliding_space(State *state, int player, int row, int col, int piece){
    static const int dr[8] = {0, 0, 1, -1, 1, 1, -1, -1};
    static const int dc[8] = {1, -1, 0, 0, 1, -1, 1, -1};
    int start = piece == 4 ? 4 : 0;
    int end = piece == 2 ? 4 : 8;
    int space = 0;

    for(int dir = start; dir < end; dir++){
        int r = row + dr[dir];
        int c = col + dc[dir];
        while(in_bounds(state, r, c)){
            if(piece_at_safe(state, player, r, c) != 0){
                break;
            }
            space++;
            if(piece_at_safe(state, 1 - player, r, c) != 0){
                break;
            }
            r += dr[dir];
            c += dc[dir];
        }
    }
    return space;
}


int side_nnue_score(State *state, int player){
    std::array<int, NNUE_HIDDEN> acc{};
    for(int i = 0; i < NNUE_HIDDEN; i++){
        acc[i] = NNUE_BIAS[i];
    }

    int opp = 1 - player;
    int own_king_r, own_king_c, opp_king_r, opp_king_c;
    bool has_own_king = find_king(state, player, own_king_r, own_king_c);
    bool has_opp_king = find_king(state, opp, opp_king_r, opp_king_c);

    for(int r = 0; r < state->board_h(); r++){
        for(int c = 0; c < state->board_w(); c++){
            int piece = piece_at_safe(state, player, r, c);
            if(piece == 0){
                continue;
            }

            int material = piece_value(piece) / 10;
            int center = center_bonus(state, r, c, piece == 1 ? 8 : 16);
            acc[0] += material;
            acc[4] += center;

            if(piece == 1){
                int dist = pawn_promotion_distance(state, player, r);
                int progress = state->board_h() - 1 - dist;
                acc[1] += progress * 5;
                if(dist <= 2){
                    acc[3] += (3 - dist) * 32;
                }
                if(is_passed_pawn(state, player, r, c)){
                    acc[2] += 28 + progress * 10;
                    if(dist <= 2){
                        acc[3] += (3 - dist) * 42;
                    }
                }
            }else if(piece == 2 || piece == 4 || piece == 5){
                acc[7] += std::min(18, sliding_space(state, player, r, c, piece) * 2);
            }else if(piece == 3){
                acc[8] += center + 4;
            }

            if(has_opp_king && piece != 6){
                int dist = std::abs(r - opp_king_r) + std::abs(c - opp_king_c);
                acc[5] += std::max(0, 7 - dist) * NNUE_TROPISM[piece];
            }

            if(has_own_king && piece != 6){
                int king_dist = std::abs(r - own_king_r) + std::abs(c - own_king_c);
                if(king_dist <= 2){
                    acc[11] += 5 - king_dist;
                }
            }

            if(piece == 5){
                acc[9] += center + 8;
            }
        }
    }

    for(int r = 0; r < state->board_h(); r++){
        for(int c = 0; c < state->board_w(); c++){
            int enemy = piece_at_safe(state, opp, r, c);
            if(enemy == 0 || enemy == 6 || !has_own_king){
                continue;
            }
            int dist = std::abs(r - own_king_r) + std::abs(c - own_king_c);
            acc[6] -= std::max(0, 7 - dist) * NNUE_TROPISM[enemy];
        }
    }

    if(has_own_king){
        int back_rank = player == 0 ? state->board_h() - 1 : 0;
        int king_home = std::abs(own_king_r - back_rank);
        acc[10] += std::max(0, 10 - king_home * 3);
    }

    int score = 0;
    for(int i = 0; i < NNUE_HIDDEN; i++){
        score += crelu(acc[i]) * NNUE_OUT[i];
    }
    return score / 24;
}


int nnue_eval_bonus(State *state){
    int self = state->player;
    int opp = 1 - self;
    return side_nnue_score(state, self) - side_nnue_score(state, opp);
}


class Searcher {
public:
    Searcher(const Submission1Params& params, SearchContext& context)
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
    Submission1Params p;
    SearchContext& ctx;
    std::vector<std::array<Move, 2>> killers;
    std::unordered_map<uint64_t, int> history_scores;


    int evaluate(State *state, GameHistory& history){
        int score = state->evaluate(p.use_kp_eval, p.use_eval_mobility, &history);
        if(score > MATE_THRESHOLD || score < -MATE_THRESHOLD){
            return score;
        }


        int self = state->player;
        int opp = 1 - self;
        if(p.use_test_eval){
            score += player_eval_bonus(state, self);
            score -= player_eval_bonus(state, opp);
        }
        if(p.use_nnue_eval && p.nnue_weight > 0){
            score += nnue_eval_bonus(state) * p.nnue_weight / 16;
        }
        CheckStatus check = inspect_check_status(state);
        if(check.mated){
            return M_MAX + 1;
        }
        if(check.in_check){
            score -= 900;
            score -= std::max(0, 3 - check.safe_evasions) * 300;
        }
        return std::max(M_MAX + 1, std::min(P_MAX - 1, score));
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


    bool can_try_null_move(State *state, int depth, int ply, int alpha, int beta){
        if(!p.use_null_move || depth < 4 || ply == 0){
            return false;
        }
        if(opponent_can_capture_king(state)){
            return false;
        }
        if(beta >= MATE_THRESHOLD || alpha <= -MATE_THRESHOLD){
            return false;
        }
        if(!has_non_pawn_material(state, state->player)){
            return false;
        }
        if(has_promotion_race_pawn(state, state->player)
           || has_promotion_race_pawn(state, 1 - state->player)){
            return false;
        }
        return true;
    }


    bool null_move_cutoff(
        State *state,
        int depth,
        GameHistory& history,
        int ply,
        int beta
    ){
        BaseState* null_base = state->create_null_state();
        if(!null_base){
            return false;
        }


        State* null_state = static_cast<State*>(null_base);
        int reduction = depth >= 6 ? 3 : 2;
        int null_depth = std::max(0, depth - 1 - reduction);
        int raw = pvs(null_state, null_depth, history, ply + 1, -beta, -beta + 1);
        int score = -raw;
        delete null_state;
        return score >= beta;
    }


    bool can_skip_by_futility(
        State *state,
        const Move& action,
        int depth,
        int static_eval,
        int alpha
    ){
        if(!p.use_futility || depth > 2 || static_eval > alpha){
            return false;
        }
        if(opponent_can_capture_king(state)){
            return false;
        }
        if(!is_quiet(state, action) || is_promotion_race(state, action)){
            return false;
        }
        if(move_gives_check(state, action)){
            return false;
        }
        int margin = 120 * depth;
        return static_eval + margin <= alpha;
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
        CheckStatus check = inspect_check_status(state);
        if(check.mated){
            history.pop(key);
            return M_MAX + ply;
        }

        bool in_check = check.in_check;
        if(!in_check){
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
        }

        auto actions = in_check
            ? ordered_actions(state, TTProbe{}, ply, killers, history_scores)
            : ordered_noisy_actions(state, p);
        for(const Move& action : actions){
            if(ctx.stop){
                break;
            }

            if(!in_check){
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

        CheckStatus check = inspect_check_status(state);
        if(check.mated){
            return M_MAX + ply;
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


        int static_eval = evaluate(state, history);
        bool in_check = check.in_check;
        if(can_try_null_move(state, depth, ply, alpha, beta)
           && static_eval >= beta
           && null_move_cutoff(state, depth, history, ply, beta)){
            history.pop(key);
            return beta;
        }


        auto actions = ordered_actions(
            state, tt, ply, killers, history_scores
        );
        if(actions.empty()){
            history.pop(key);
            return static_eval;
        }


        int best_score = M_MAX;
        Move best_move = NO_MOVE;
        bool first_child = true;


        for(int move_index = 0; move_index < static_cast<int>(actions.size()); move_index++){
            const Move& action = actions[move_index];
            bool quiet = is_quiet(state, action);
            if(!first_child
               && can_skip_by_futility(state, action, depth, static_eval, alpha)){
                continue;
            }


            State* next = state->next_state(action);
            bool same = next->same_player_as_parent();
            int score;


            if(first_child){
                score = search_child(
                    next, depth - 1, history, ply + 1, alpha, beta, same
                );
                first_child = false;
            }else{
                int child_depth = depth - 1;
                bool tt_move = tt.has_move && same_move(action, tt.move);
                bool gives_check = move_gives_check(state, action);
                bool can_lmr = p.use_lmr
                    && depth >= 3
                    && move_index >= 3
                    && !in_check
                    && quiet
                    && !gives_check
                    && !is_promotion_race(state, action)
                    && !tt_move;


                if(can_lmr){
                    int reduction = (depth >= 6 && move_index >= 6) ? 2 : 1;
                    int reduced_depth = std::max(0, child_depth - reduction);
                    score = search_child(
                        next, reduced_depth, history, ply + 1,
                        alpha, alpha + 1, same
                    );
                    if(score > alpha && !ctx.stop){
                        score = search_child(
                            next, child_depth, history, ply + 1,
                            alpha, alpha + 1, same
                        );
                    }
                }else{
                    score = search_child(
                        next, child_depth, history, ply + 1,
                        alpha, alpha + 1, same
                    );
                }


                if(score > alpha && score < beta && !ctx.stop){
                    score = search_child(
                        next, child_depth, history, ply + 1, alpha, beta, same
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
                if(quiet){
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
                int child_depth = depth - 1;
                bool quiet = is_quiet(state, action);
                bool tt_move = tt.has_move && same_move(action, tt.move);
                bool can_lmr = p.use_lmr
                    && depth >= 3
                    && move_index >= 3
                    && quiet
                    && !is_promotion_race(state, action)
                    && !tt_move;


                if(can_lmr){
                    int reduction = (depth >= 6 && move_index >= 6) ? 2 : 1;
                    int reduced_depth = std::max(0, child_depth - reduction);
                    score = search_child(
                        next, reduced_depth, history, 1,
                        alpha, alpha + 1, same
                    );
                    if(score > alpha && !ctx.stop){
                        score = search_child(
                            next, child_depth, history, 1,
                            alpha, alpha + 1, same
                        );
                    }
                }else{
                    score = search_child(
                        next, child_depth, history, 1,
                        alpha, alpha + 1, same
                    );
                }


                if(score > alpha && score < beta && !ctx.stop){
                    score = search_child(
                        next, child_depth, history, 1, alpha, beta, same
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


SearchResult Submission1::search(
    State *state,
    int depth,
    GameHistory& history,
    SearchContext& ctx
){
    ctx.reset();
    Submission1Params p = Submission1Params::from_map(ctx.params);
    configure_tt(p);


    Searcher searcher(p, ctx);
    return searcher.run(state, depth, history);
}


ParamMap Submission1::default_params(){
    return {
        {"UseKPEval", "true"},
        {"UseEvalMobility", "true"},
        {"ReportPartial", "false"},
        {"UseTT", "true"},
        {"TTSizeMB", "32"},
        {"UseAspiration", "true"},
        {"AspirationWindow", "50"},
        {"UseLMR", "false"},
        {"UseNullMove", "true"},
        {"UseFutility", "false"},
        {"UseTestEval", "true"},
        {"UseNNUEEval", "false"},
        {"NNUEWeight", "0"},
        {"QuiescenceMaxPly", "8"},
        {"QuiescencePromotions", "true"},
    };
}


std::vector<ParamDef> Submission1::param_defs(){
    return {
        {"UseKPEval", ParamDef::CHECK, "true"},
        {"UseEvalMobility", ParamDef::CHECK, "true"},
        {"ReportPartial", ParamDef::CHECK, "false"},
        {"UseTT", ParamDef::CHECK, "true"},
        {"TTSizeMB", ParamDef::SPIN, "32", 1, 256},
        {"UseAspiration", ParamDef::CHECK, "true"},
        {"AspirationWindow", ParamDef::SPIN, "50", 1, 1000},
        {"UseLMR", ParamDef::CHECK, "false"},
        {"UseNullMove", ParamDef::CHECK, "true"},
        {"UseFutility", ParamDef::CHECK, "false"},
        {"UseTestEval", ParamDef::CHECK, "true"},
        {"UseNNUEEval", ParamDef::CHECK, "false"},
        {"NNUEWeight", ParamDef::SPIN, "0", 0, 64},
        {"QuiescenceMaxPly", ParamDef::SPIN, "8", 0, 24},
        {"QuiescencePromotions", ParamDef::CHECK, "true"},
    };
}
