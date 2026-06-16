#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "state.hpp"
#include "config.hpp"
#include "test.hpp"

namespace {

constexpr int MAX_PLY = 128;
constexpr uint16_t INVALID_MOVE = 0xffff;

constexpr uint8_t TT_EXACT = 0;
constexpr uint8_t TT_LOWER = 1;
constexpr uint8_t TT_UPPER = 2;

struct TTEntry {
    uint64_t key = 0;
    int score = 0;
    int16_t depth = -1;
    uint16_t move = INVALID_MOVE;
    uint8_t flag = TT_EXACT;
    uint8_t age = 0;
};

struct OrderedMove {
    Move move;
    int score = 0;
    bool noisy = false;
    uint16_t enc = INVALID_MOVE;
};

std::vector<TTEntry> g_tt;
uint8_t g_age = 1;
bool g_heur_ready = false;
uint16_t g_killers[MAX_PLY][2];
int g_history_bonus[2][64][64];

static const int MATERIAL[7] = {0, 100, 500, 320, 340, 950, 0};
static const int END_MATERIAL[7] = {0, 2, 6, 7, 8, 20, 0};

static const int PST[7][BOARD_H][BOARD_W] = {
    {{0, 0, 0, 0, 0}, {0, 0, 0, 0, 0}, {0, 0, 0, 0, 0},
     {0, 0, 0, 0, 0}, {0, 0, 0, 0, 0}, {0, 0, 0, 0, 0}},

    {{0, 0, 0, 0, 0}, {90, 95, 105, 95, 90}, {30, 38, 45, 38, 30},
     {14, 20, 26, 20, 14}, {4, 8, 10, 8, 4}, {0, 0, 0, 0, 0}},

    {{8, 10, 14, 10, 8}, {12, 18, 22, 18, 12}, {0, 8, 14, 8, 0},
     {0, 8, 14, 8, 0}, {4, 8, 10, 8, 4}, {0, 0, 0, 0, 0}},

    {{-20, -8, 0, -8, -20}, {-6, 16, 26, 16, -6}, {6, 30, 40, 30, 6},
     {6, 30, 40, 30, 6}, {-6, 16, 26, 16, -6}, {-20, -8, 0, -8, -20}},

    {{-8, 0, 4, 0, -8}, {0, 18, 24, 18, 0}, {6, 24, 32, 24, 6},
     {6, 24, 32, 24, 6}, {0, 18, 24, 18, 0}, {-8, 0, 4, 0, -8}},

    {{-4, 4, 10, 4, -4}, {6, 18, 28, 18, 6}, {10, 28, 40, 28, 10},
     {10, 28, 40, 28, 10}, {6, 18, 28, 18, 6}, {-4, 4, 10, 4, -4}},

    {{-40, -35, -35, -35, -40}, {-25, -18, -18, -18, -25},
     {-15, -10, -8, -10, -15}, {-8, 0, 4, 0, -8},
     {16, 24, 12, 24, 16}, {28, 32, 18, 32, 28}},
};

void init_heuristics()
{
    if(g_heur_ready){
        return;
    }
    for(int ply = 0; ply < MAX_PLY; ply++){
        g_killers[ply][0] = INVALID_MOVE;
        g_killers[ply][1] = INVALID_MOVE;
    }
    g_heur_ready = true;
}

int clamp_int(int value, int lo, int hi)
{
    return std::max(lo, std::min(value, hi));
}

void ensure_tt(int hash_mb)
{
    hash_mb = clamp_int(hash_mb, 1, 512);
    size_t target_bytes = static_cast<size_t>(hash_mb) * 1024u * 1024u;
    size_t n = 1;
    while((n << 1) * sizeof(TTEntry) <= target_bytes){
        n <<= 1;
    }
    n = std::max<size_t>(n, 1024);
    if(g_tt.size() != n){
        g_tt.assign(n, TTEntry{});
    }
}

int sq_index(int row, int col, int w)
{
    return row * w + col;
}

uint16_t encode_move(const State* state, const Move& move)
{
    int h = state->board_h();
    int w = state->board_w();
    if(move.first.first >= static_cast<size_t>(h)
       || move.first.second >= static_cast<size_t>(w)
       || move.second.second >= static_cast<size_t>(w)){
        return INVALID_MOVE;
    }

    int from = sq_index(
        static_cast<int>(move.first.first),
        static_cast<int>(move.first.second),
        w
    );
    int to_row = static_cast<int>(move.second.first);
    if(to_row >= h){
        to_row %= h;
    }
    if(to_row < 0 || to_row >= h){
        return INVALID_MOVE;
    }
    int to = sq_index(to_row, static_cast<int>(move.second.second), w);
    return static_cast<uint16_t>((from << 6) | to);
}

bool same_move(uint16_t enc, const State* state, const Move& move)
{
    return enc != INVALID_MOVE && enc == encode_move(state, move);
}

int moving_piece(const State* state, const Move& move)
{
    if(move.first.first >= static_cast<size_t>(state->board_h())
       || move.first.second >= static_cast<size_t>(state->board_w())){
        return 0;
    }
    return state->piece_at(
        state->player,
        static_cast<int>(move.first.first),
        static_cast<int>(move.first.second)
    );
}

int captured_piece(const State* state, const Move& move)
{
    int h = state->board_h();
    int w = state->board_w();
    if(move.second.second >= static_cast<size_t>(w)){
        return 0;
    }
    int row = static_cast<int>(move.second.first);
    if(row >= h){
        row %= h;
    }
    if(row < 0 || row >= h){
        return 0;
    }
    return state->piece_at(1 - state->player, row, static_cast<int>(move.second.second));
}

bool is_promotion(const State* state, const Move& move)
{
    int piece = moving_piece(state, move);
    if(piece != 1){
        return false;
    }
    int h = state->board_h();
    int row = static_cast<int>(move.second.first);
    if(row >= h){
        row %= h;
    }
    return row == 0 || row == h - 1;
}

bool is_noisy_move(const State* state, const Move& move)
{
    return captured_piece(state, move) > 0 || is_promotion(state, move);
}

bool occupied_at(const State* state, int row, int col)
{
    return state->piece_at(0, row, col) || state->piece_at(1, row, col);
}

bool clear_line(const State* state, int r, int c, int tr, int tc)
{
    int dr = (tr > r) - (tr < r);
    int dc = (tc > c) - (tc < c);
    r += dr;
    c += dc;
    while(r != tr || c != tc){
        if(occupied_at(state, r, c)){
            return false;
        }
        r += dr;
        c += dc;
    }
    return true;
}

bool piece_attacks_square(
    const State* state,
    int owner,
    int piece,
    int r,
    int c,
    int tr,
    int tc
){
    int dr = tr - r;
    int dc = tc - c;
    int adr = std::abs(dr);
    int adc = std::abs(dc);

    switch(piece){
        case 1:
            return owner == 0
                ? (dr == -1 && adc == 1)
                : (dr == 1 && adc == 1);
        case 2:
            return (dr == 0 || dc == 0) && clear_line(state, r, c, tr, tc);
        case 3:
            return (adr == 1 && adc == 2) || (adr == 2 && adc == 1);
        case 4:
            return adr == adc && adr > 0 && clear_line(state, r, c, tr, tc);
        case 5:
            return ((dr == 0 || dc == 0) || (adr == adc && adr > 0))
                && clear_line(state, r, c, tr, tc);
        case 6:
            return std::max(adr, adc) == 1;
        default:
            return false;
    }
}

bool attacks_square(const State* state, int owner, int tr, int tc)
{
    for(int r = 0; r < state->board_h(); r++){
        for(int c = 0; c < state->board_w(); c++){
            int piece = state->piece_at(owner, r, c);
            if(piece && piece_attacks_square(state, owner, piece, r, c, tr, tc)){
                return true;
            }
        }
    }
    return false;
}

bool find_king(const State* state, int owner, int& kr, int& kc)
{
    for(int r = 0; r < state->board_h(); r++){
        for(int c = 0; c < state->board_w(); c++){
            if(state->piece_at(owner, r, c) == 6){
                kr = r;
                kc = c;
                return true;
            }
        }
    }
    kr = kc = -1;
    return false;
}

int endgame_material(const State* state, int owner)
{
    int score = 0;
    for(int r = 0; r < state->board_h(); r++){
        for(int c = 0; c < state->board_w(); c++){
            int piece = state->piece_at(owner, r, c);
            if(piece >= 1 && piece <= 5){
                score += END_MATERIAL[piece];
            }
        }
    }
    return score;
}

int pseudo_mobility(const State* state, int owner)
{
    int h = state->board_h();
    int w = state->board_w();
    int count = 0;

    auto empty_or_enemy = [&](int r, int c){
        return r >= 0 && r < h && c >= 0 && c < w
            && !state->piece_at(owner, r, c);
    };

    static const int knight_dr[8] = {1, 1, -1, -1, 2, 2, -2, -2};
    static const int knight_dc[8] = {2, -2, 2, -2, 1, -1, 1, -1};
    static const int king_dr[8] = {1, 0, -1, 0, 1, 1, -1, -1};
    static const int king_dc[8] = {0, 1, 0, -1, 1, -1, 1, -1};
    static const int slide_dr[8] = {0, 0, 1, -1, 1, 1, -1, -1};
    static const int slide_dc[8] = {1, -1, 0, 0, 1, -1, 1, -1};

    for(int r = 0; r < h; r++){
        for(int c = 0; c < w; c++){
            int piece = state->piece_at(owner, r, c);
            if(!piece){
                continue;
            }
            switch(piece){
                case 1: {
                    int nr = r + (owner == 0 ? -1 : 1);
                    if(nr >= 0 && nr < h){
                        if(!occupied_at(state, nr, c)){
                            count++;
                        }
                        if(c > 0 && state->piece_at(1 - owner, nr, c - 1)){
                            count++;
                        }
                        if(c + 1 < w && state->piece_at(1 - owner, nr, c + 1)){
                            count++;
                        }
                    }
                    break;
                }
                case 3:
                    for(int i = 0; i < 8; i++){
                        count += empty_or_enemy(r + knight_dr[i], c + knight_dc[i]) ? 1 : 0;
                    }
                    break;
                case 6:
                    for(int i = 0; i < 8; i++){
                        count += empty_or_enemy(r + king_dr[i], c + king_dc[i]) ? 1 : 0;
                    }
                    break;
                case 2:
                case 4:
                case 5: {
                    int start = piece == 4 ? 4 : 0;
                    int end = piece == 2 ? 4 : 8;
                    for(int d = start; d < end; d++){
                        int nr = r + slide_dr[d];
                        int nc = c + slide_dc[d];
                        while(nr >= 0 && nr < h && nc >= 0 && nc < w){
                            if(state->piece_at(owner, nr, nc)){
                                break;
                            }
                            count++;
                            if(state->piece_at(1 - owner, nr, nc)){
                                break;
                            }
                            nr += slide_dr[d];
                            nc += slide_dc[d];
                        }
                    }
                    break;
                }
            }
        }
    }

    return count;
}

int side_value(const State* state, int owner, int enemy_kr, int enemy_kc)
{
    int h = state->board_h();
    int w = state->board_w();
    int score = 0;
    static const int tropism[7] = {0, 6, 11, 15, 13, 22, 0};

    for(int r = 0; r < h; r++){
        for(int c = 0; c < w; c++){
            int piece = state->piece_at(owner, r, c);
            if(!piece){
                continue;
            }

            int value = MATERIAL[piece];
            int rr = owner == 0 ? r : (h - 1 - r);
            if(h == BOARD_H && w == BOARD_W){
                value += PST[piece][rr][c];
            }else{
                int center_distance = std::abs(2 * r - (h - 1))
                    + std::abs(2 * c - (w - 1));
                value += std::max(0, 18 - 3 * center_distance);
            }

            if(piece == 1){
                int progress = owner == 0 ? (h - 1 - r) : r;
                value += progress * 14;
                if((owner == 0 && r == 1) || (owner == 1 && r == h - 2)){
                    value += 80;
                }
            }

            if(enemy_kr >= 0){
                int dist = std::max(std::abs(r - enemy_kr), std::abs(c - enemy_kc));
                value += tropism[piece] * std::max(0, 5 - dist);
            }

            score += value;
        }
    }

    return score;
}

int static_eval(State* state, const TestParams& p, const GameHistory* history)
{
    if(state->game_state == WIN){
        return P_MAX;
    }

    int self = state->player;
    int opp = 1 - self;
    int self_kr = -1, self_kc = -1, opp_kr = -1, opp_kc = -1;
    bool self_has_king = find_king(state, self, self_kr, self_kc);
    bool opp_has_king = find_king(state, opp, opp_kr, opp_kc);
    if(!self_has_king){
        return M_MAX;
    }
    if(!opp_has_king){
        return P_MAX;
    }

#ifdef MAX_STEP
    if(state->step > MAX_STEP){
        int self_end = endgame_material(state, self);
        int opp_end = endgame_material(state, opp);
        if(self_end > opp_end){
            return P_MAX / 2 + self_end - opp_end;
        }
        if(self_end < opp_end){
            return M_MAX / 2 - (opp_end - self_end);
        }
        return 0;
    }
#endif

    int score = 0;
    if(p.use_kp_eval){
        score = side_value(state, self, opp_kr, opp_kc)
            - side_value(state, opp, self_kr, self_kc);
    }else{
        score = endgame_material(state, self) * 100
            - endgame_material(state, opp) * 100;
    }

    if(attacks_square(state, opp, self_kr, self_kc)){
        score -= 520;
    }
    if(attacks_square(state, self, opp_kr, opp_kc)){
        score += 180;
    }

    if(p.use_eval_mobility){
        score += 3 * (pseudo_mobility(state, self) - pseudo_mobility(state, opp));
    }

    if(history){
        int repeated = history->count(state->hash());
        if(repeated >= 3){
            return 0;
        }
        if(repeated >= 2){
            score += score > 0 ? -120 : 60;
        }
    }

    return clamp_int(score, M_MAX + 1000, P_MAX - 1000);
}

bool has_non_pawn_material(const State* state, int owner)
{
    for(int r = 0; r < state->board_h(); r++){
        for(int c = 0; c < state->board_w(); c++){
            int piece = state->piece_at(owner, r, c);
            if(piece >= 2 && piece <= 5){
                return true;
            }
        }
    }
    return false;
}

uint64_t tt_key(const State* state)
{
    uint64_t key = state->hash();
    key ^= 0x9e3779b97f4a7c15ULL
        * static_cast<uint64_t>(state->step + 1);
    return key ? key : 0x6a09e667f3bcc909ULL;
}

int score_to_tt(int score, int ply)
{
    if(score > P_MAX - 1000){
        return score + ply;
    }
    if(score < M_MAX + 1000){
        return score - ply;
    }
    return score;
}

int score_from_tt(int score, int ply)
{
    if(score > P_MAX - 1000){
        return score - ply;
    }
    if(score < M_MAX + 1000){
        return score + ply;
    }
    return score;
}

TTEntry* probe_tt(uint64_t key)
{
    if(g_tt.empty()){
        return nullptr;
    }
    TTEntry& entry = g_tt[key & (g_tt.size() - 1)];
    return entry.key == key ? &entry : nullptr;
}

void store_tt(
    uint64_t key,
    int depth,
    int score,
    uint8_t flag,
    uint16_t move,
    int ply
){
    if(g_tt.empty()){
        return;
    }
    TTEntry& entry = g_tt[key & (g_tt.size() - 1)];
    if(entry.key != key && entry.key != 0 && entry.depth > depth + 2 && entry.age == g_age){
        return;
    }
    entry.key = key;
    entry.depth = static_cast<int16_t>(depth);
    entry.score = score_to_tt(score, ply);
    entry.flag = flag;
    entry.move = move;
    entry.age = g_age;
}

void remember_killer(int ply, uint16_t enc, int slots)
{
    if(enc == INVALID_MOVE || ply < 0 || ply >= MAX_PLY || slots <= 0){
        return;
    }
    slots = clamp_int(slots, 1, 2);
    if(g_killers[ply][0] == enc){
        return;
    }
    if(slots > 1){
        g_killers[ply][1] = g_killers[ply][0];
    }
    g_killers[ply][0] = enc;
}

void add_history_bonus(int player, uint16_t enc, int depth)
{
    if(enc == INVALID_MOVE){
        return;
    }
    int from = enc >> 6;
    int to = enc & 63;
    if(player < 0 || player > 1 || from >= 64 || to >= 64){
        return;
    }
    int& value = g_history_bonus[player][from][to];
    value += depth * depth + 1;
    if(value > 1000000){
        value /= 2;
    }
}

std::vector<OrderedMove> order_moves(
    State* state,
    const std::vector<Move>& actions,
    uint16_t tt_move,
    int ply,
    const TestParams& p
){
    std::vector<OrderedMove> ordered;
    ordered.reserve(actions.size());

    for(const Move& move : actions){
        OrderedMove om;
        om.move = move;
        om.enc = encode_move(state, move);
        om.noisy = is_noisy_move(state, move);

        if(!p.use_move_ordering){
            ordered.push_back(om);
            continue;
        }

        if(same_move(tt_move, state, move)){
            om.score += 2000000;
        }

        int attacker = moving_piece(state, move);
        int victim = captured_piece(state, move);
        if(victim){
            om.score += 1000000 + PIECE_VALUES[victim] * 128 - PIECE_VALUES[attacker] * 8;
            if(victim == 6){
                om.score += 5000000;
            }
        }
        if(is_promotion(state, move)){
            om.score += 120000;
        }

        if(!om.noisy && p.use_killer_moves && ply >= 0 && ply < MAX_PLY){
            if(g_killers[ply][0] == om.enc){
                om.score += 90000;
            }else if(p.killer_slots > 1 && g_killers[ply][1] == om.enc){
                om.score += 80000;
            }
        }

        if(!om.noisy && om.enc != INVALID_MOVE){
            int from = om.enc >> 6;
            int to = om.enc & 63;
            if(from < 64 && to < 64){
                om.score += g_history_bonus[state->player][from][to];
            }
        }

        ordered.push_back(om);
    }

    if(p.use_move_ordering){
        std::stable_sort(
            ordered.begin(),
            ordered.end(),
            [](const OrderedMove& a, const OrderedMove& b){
                return a.score > b.score;
            }
        );
    }

    return ordered;
}

int qsearch(
    State* state,
    int alpha,
    int beta,
    GameHistory& history,
    int ply,
    int qply,
    SearchContext& ctx,
    const TestParams& p
);

int negamax(
    State* state,
    int depth,
    int alpha,
    int beta,
    GameHistory& history,
    int ply,
    SearchContext& ctx,
    const TestParams& p,
    bool pv_node,
    bool allow_null
);

int search_child(
    State* child,
    bool same_player,
    int depth,
    int alpha,
    int beta,
    GameHistory& history,
    int ply,
    SearchContext& ctx,
    const TestParams& p,
    bool pv_node,
    bool allow_null
){
    if(same_player){
        return negamax(
            child, depth, alpha, beta, history, ply, ctx, p, pv_node, allow_null
        );
    }
    return -negamax(
        child, depth, -beta, -alpha, history, ply, ctx, p, pv_node, allow_null
    );
}

int qsearch(
    State* state,
    int alpha,
    int beta,
    GameHistory& history,
    int ply,
    int qply,
    SearchContext& ctx,
    const TestParams& p
){
    ctx.nodes++;
    if(ply > ctx.seldepth){
        ctx.seldepth = ply;
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

    int rep_score = 0;
    if(state->check_repetition(history, rep_score)){
        return rep_score;
    }

    int stand_pat = static_eval(state, p, &history);
    if(stand_pat >= beta){
        return stand_pat;
    }
    if(stand_pat > alpha){
        alpha = stand_pat;
    }
    if(qply >= p.quiescence_max_depth || ply >= MAX_PLY - 1){
        return alpha;
    }

    history.push(state->hash());

    auto ordered = order_moves(state, state->legal_actions, INVALID_MOVE, ply, p);
    for(const OrderedMove& om : ordered){
        if(!om.noisy){
            continue;
        }

        State* child = state->next_state(om.move);
        bool same = child->same_player_as_parent();
        int score = same
            ? qsearch(child, alpha, beta, history, ply + 1, qply + 1, ctx, p)
            : -qsearch(child, -beta, -alpha, history, ply + 1, qply + 1, ctx, p);
        delete child;

        if(score > alpha){
            alpha = score;
            if(alpha >= beta){
                history.pop(state->hash());
                return alpha;
            }
        }
    }

    history.pop(state->hash());
    return alpha;
}

int negamax(
    State* state,
    int depth,
    int alpha,
    int beta,
    GameHistory& history,
    int ply,
    SearchContext& ctx,
    const TestParams& p,
    bool pv_node,
    bool allow_null
){
    ctx.nodes++;
    if(ply > ctx.seldepth){
        ctx.seldepth = ply;
    }
    if(ply >= MAX_PLY - 1){
        return static_eval(state, p, &history);
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

    int rep_score = 0;
    if(state->check_repetition(history, rep_score)){
        return rep_score;
    }

    if(depth <= 0){
        if(p.use_quiescence){
            return qsearch(state, alpha, beta, history, ply, 0, ctx, p);
        }
        return static_eval(state, p, &history);
    }

    bool history_sensitive = history.count(state->hash()) > 0;
    uint64_t key = tt_key(state);
    uint16_t tt_move = INVALID_MOVE;
    int alpha_orig = alpha;

    if(p.use_tt && !history_sensitive){
        if(TTEntry* entry = probe_tt(key)){
            tt_move = entry->move;
            if(entry->depth >= depth){
                int tt_score = score_from_tt(entry->score, ply);
                if(entry->flag == TT_EXACT){
                    return tt_score;
                }
                if(entry->flag == TT_LOWER && tt_score >= beta){
                    return tt_score;
                }
                if(entry->flag == TT_UPPER && tt_score <= alpha){
                    return tt_score;
                }
            }
        }
    }

    int static_score = static_eval(state, p, &history);
    history.push(state->hash());

    if(p.use_null_move
       && allow_null
       && !pv_node
       && depth >= p.null_move_r + 2
       && static_score >= beta
       && has_non_pawn_material(state, state->player)){
        BaseState* base_null = state->create_null_state();
        State* null_state = static_cast<State*>(base_null);
        if(null_state){
            int reduction = p.null_move_r + (depth >= 6 ? 1 : 0);
            int null_depth = std::max(0, depth - 1 - reduction);
            int score = -negamax(
                null_state,
                null_depth,
                -beta,
                -beta + 1,
                history,
                ply + 1,
                ctx,
                p,
                false,
                false
            );
            delete null_state;
            if(score >= beta){
                history.pop(state->hash());
                return score;
            }
        }
    }

    auto ordered = order_moves(state, state->legal_actions, tt_move, ply, p);
    if(ordered.empty()){
        history.pop(state->hash());
        return static_score;
    }

    int best_score = M_MAX;
    uint16_t best_move = INVALID_MOVE;
    int searched = 0;

    for(const OrderedMove& om : ordered){
        State* child = state->next_state(om.move);
        bool same = child->same_player_as_parent();
        int child_depth = depth - 1;
        int score = M_MAX;

        if(searched == 0){
            score = search_child(
                child,
                same,
                child_depth,
                alpha,
                beta,
                history,
                ply + 1,
                ctx,
                p,
                pv_node,
                true
            );
        }else{
            bool reduce = p.use_lmr
                && !pv_node
                && !om.noisy
                && depth >= p.lmr_depth_limit
                && searched >= p.lmr_full_depth;

            if(reduce){
                int reduction = 1 + (depth >= 6 && searched >= 6 ? 1 : 0);
                int reduced_depth = std::max(0, child_depth - reduction);
                score = search_child(
                    child,
                    same,
                    reduced_depth,
                    alpha,
                    alpha + 1,
                    history,
                    ply + 1,
                    ctx,
                    p,
                    false,
                    true
                );
                if(score > alpha){
                    score = search_child(
                        child,
                        same,
                        child_depth,
                        alpha,
                        alpha + 1,
                        history,
                        ply + 1,
                        ctx,
                        p,
                        false,
                        true
                    );
                }
            }else{
                score = search_child(
                    child,
                    same,
                    child_depth,
                    alpha,
                    alpha + 1,
                    history,
                    ply + 1,
                    ctx,
                    p,
                    false,
                    true
                );
            }

            if(score > alpha && score < beta){
                score = search_child(
                    child,
                    same,
                    child_depth,
                    alpha,
                    beta,
                    history,
                    ply + 1,
                    ctx,
                    p,
                    true,
                    true
                );
            }
        }

        delete child;
        searched++;

        if(score > best_score){
            best_score = score;
            best_move = om.enc;
        }
        if(score > alpha){
            alpha = score;
        }
        if(alpha >= beta){
            if(!om.noisy){
                if(p.use_killer_moves){
                    remember_killer(ply, om.enc, p.killer_slots);
                }
                add_history_bonus(state->player, om.enc, depth);
            }
            break;
        }
    }

    history.pop(state->hash());

    if(p.use_tt && !history_sensitive){
        uint8_t flag = TT_EXACT;
        if(best_score <= alpha_orig){
            flag = TT_UPPER;
        }else if(best_score >= beta){
            flag = TT_LOWER;
        }
        store_tt(key, depth, best_score, flag, best_move, ply);
    }

    return best_score;
}

} // namespace

TestParams TestParams::from_map(const ParamMap& m)
{
    TestParams p;
    p.use_kp_eval = param_bool(m, "UseKPEval", true);
    p.use_eval_mobility = param_bool(m, "UseEvalMobility", true);
    p.use_quiescence = param_bool(m, "UseQuiescence", true);
    p.use_move_ordering = param_bool(m, "UseMoveOrdering", true);
    p.use_tt = param_bool(m, "UseTT", true);
    p.use_null_move = param_bool(m, "UseNullMove", true);
    p.use_killer_moves = param_bool(m, "UseKillerMoves", true);
    p.use_lmr = param_bool(m, "UseLMR", true);
    p.report_partial = param_bool(m, "ReportPartial", true);

    p.hash_mb = clamp_int(param_int(m, "Hash", 24), 1, 512);
    p.quiescence_max_depth = clamp_int(param_int(m, "QuiescenceMaxDepth", 16), 0, 32);
    p.killer_slots = clamp_int(param_int(m, "KillerSlots", 2), 1, 2);
    p.null_move_r = clamp_int(param_int(m, "NullMoveR", 2), 1, 4);
    p.lmr_full_depth = clamp_int(param_int(m, "LMRFullDepth", 3), 1, 12);
    p.lmr_depth_limit = clamp_int(param_int(m, "LMRDepthLimit", 3), 2, 12);
    return p;
}

SearchResult Test::search(
    State* state,
    int depth,
    GameHistory& history,
    SearchContext& ctx
){
    init_heuristics();
    TestParams p = TestParams::from_map(ctx.params);
    ensure_tt(p.hash_mb);
    g_age++;
    if(g_age == 0){
        g_age = 1;
    }

    ctx.reset();

    SearchResult result;
    result.depth = depth;

    if(state->legal_actions.empty() && state->game_state == UNKNOWN){
        state->get_legal_actions();
    }

    if(state->legal_actions.empty()){
        result.best_move = Move();
        result.score = static_eval(state, p, &history);
        result.nodes = ctx.nodes;
        result.seldepth = ctx.seldepth;
        return result;
    }

    if(state->game_state == WIN){
        result.best_move = state->legal_actions[0];
        result.score = P_MAX;
        result.nodes = 1;
        result.seldepth = 1;
        result.pv = {result.best_move};
        return result;
    }

    uint16_t tt_move = INVALID_MOVE;
    uint64_t key = tt_key(state);
    if(p.use_tt){
        if(TTEntry* entry = probe_tt(key)){
            tt_move = entry->move;
        }
    }

    auto ordered = order_moves(state, state->legal_actions, tt_move, 0, p);

    int alpha = M_MAX;
    int beta = P_MAX;
    int best_score = M_MAX;
    Move best_move = ordered[0].move;
    uint16_t best_enc = ordered[0].enc;
    int total_moves = static_cast<int>(ordered.size());
    int searched = 0;

    for(const OrderedMove& om : ordered){
        State* child = state->next_state(om.move);
        bool same = child->same_player_as_parent();
        int child_depth = std::max(0, depth - 1);
        int score = M_MAX;

        if(searched == 0){
            score = search_child(
                child,
                same,
                child_depth,
                alpha,
                beta,
                history,
                1,
                ctx,
                p,
                true,
                true
            );
        }else{
            score = search_child(
                child,
                same,
                child_depth,
                alpha,
                alpha + 1,
                history,
                1,
                ctx,
                p,
                false,
                true
            );
            if(score > alpha && score < beta){
                score = search_child(
                    child,
                    same,
                    child_depth,
                    alpha,
                    beta,
                    history,
                    1,
                    ctx,
                    p,
                    true,
                    true
                );
            }
        }

        delete child;
        searched++;

        if(score > best_score){
            best_score = score;
            best_move = om.move;
            best_enc = om.enc;
            if(p.report_partial && ctx.on_root_update){
                ctx.on_root_update({best_move, best_score, depth, searched, total_moves});
            }
        }
        if(score > alpha){
            alpha = score;
        }
        if(best_score >= P_MAX - 100){
            break;
        }
    }

    if(p.use_tt){
        store_tt(key, depth, best_score, TT_EXACT, best_enc, 0);
    }

    result.best_move = best_move;
    result.score = best_score;
    result.nodes = ctx.nodes;
    result.seldepth = ctx.seldepth;
    result.pv = {best_move};
    return result;
}

ParamMap Test::default_params()
{
    return {
        {"UseKPEval", "true"},
        {"UseEvalMobility", "true"},
        {"UseQuiescence", "true"},
        {"UseMoveOrdering", "true"},
        {"UseTT", "true"},
        {"UseNullMove", "true"},
        {"UseKillerMoves", "true"},
        {"UseLMR", "true"},
        {"ReportPartial", "true"},
        {"Hash", "24"},
        {"QuiescenceMaxDepth", "16"},
        {"KillerSlots", "2"},
        {"NullMoveR", "2"},
        {"LMRFullDepth", "3"},
        {"LMRDepthLimit", "3"},
    };
}

std::vector<ParamDef> Test::param_defs()
{
    return {
        {"UseKPEval", ParamDef::CHECK, "true"},
        {"UseEvalMobility", ParamDef::CHECK, "true"},
        {"UseQuiescence", ParamDef::CHECK, "true"},
        {"UseMoveOrdering", ParamDef::CHECK, "true"},
        {"UseTT", ParamDef::CHECK, "true"},
        {"UseNullMove", ParamDef::CHECK, "true"},
        {"UseKillerMoves", ParamDef::CHECK, "true"},
        {"UseLMR", ParamDef::CHECK, "true"},
        {"ReportPartial", ParamDef::CHECK, "true"},
        {"Hash", ParamDef::SPIN, "24", 1, 512},
        {"QuiescenceMaxDepth", ParamDef::SPIN, "16", 0, 32},
        {"KillerSlots", ParamDef::SPIN, "2", 1, 2},
        {"NullMoveR", ParamDef::SPIN, "2", 1, 4},
        {"LMRFullDepth", ParamDef::SPIN, "3", 1, 12},
        {"LMRDepthLimit", ParamDef::SPIN, "3", 2, 12},
    };
}
