#include <algorithm>
#include <array>
#include <chrono>
#include <utility>
#include <vector>

#include "114062127_state.hpp"
#include "114062127_submission.hpp"


// MiniMax / Alpha-Beta / PVS / Quiescence
// CLI name "minimax" kept for compatibility; implementation is fail-soft negamax:
//   alpha-beta pruning, principal variation search (PVS),
//   tactical quiescence at the horizon, capture/promotion move ordering.

struct ScoredMove {
    Move move;
    int score;
};

constexpr int TT_BITS = 20;
constexpr int TT_SIZE = 1 << TT_BITS;
constexpr int MAX_PLY_LOCAL = 64;
constexpr int HIST_SQ = BOARD_H * BOARD_W;

enum TTFlag { TT_EXACT = 0, TT_LOWER = 1, TT_UPPER = 2 };

struct TTEntry {
    uint64_t key = 0;
    int depth = -1;
    int score = 0;
    TTFlag flag = TT_EXACT;
    Move best_move = Move();
    bool used = false;
};

static std::array<TTEntry, TT_SIZE> g_tt;
static Move g_killers[MAX_PLY_LOCAL][2];
static bool g_killer_used[MAX_PLY_LOCAL][2] = {};
static int g_history[2][HIST_SQ][HIST_SQ] = {};

static void clear_search_tables(){
    for(auto& entry : g_tt){
        entry.used = false;
        entry.depth = -1;
    }
    for(int p = 0; p < MAX_PLY_LOCAL; p++){
        g_killer_used[p][0] = false;
        g_killer_used[p][1] = false;
    }
    for(int pl = 0; pl < 2; pl++){
        for(int a = 0; a < HIST_SQ; a++){
            for(int b = 0; b < HIST_SQ; b++){
                g_history[pl][a][b] = 0;
            }
        }
    }
}

static TTEntry* tt_probe(uint64_t key){
    TTEntry& entry = g_tt[key & (TT_SIZE - 1)];
    if(entry.used && entry.key == key){
        return &entry;
    }
    return nullptr;
}

static void tt_store(uint64_t key, int score, int depth, TTFlag flag, const Move& best_move){
    TTEntry& entry = g_tt[key & (TT_SIZE - 1)];
    if(!entry.used || depth >= entry.depth){
        entry.used = true;
        entry.key = key;
        entry.score = score;
        entry.depth = depth;
        entry.flag = flag;
        entry.best_move = best_move;
    }
}

static bool killer_match(int ply, const Move& move){
    if(ply < 0 || ply >= MAX_PLY_LOCAL){
        return false;
    }
    return (g_killer_used[ply][0] && g_killers[ply][0] == move)
        || (g_killer_used[ply][1] && g_killers[ply][1] == move);
}

static void killer_store(int ply, const Move& move){
    if(ply < 0 || ply >= MAX_PLY_LOCAL){
        return;
    }
    if(g_killer_used[ply][0] && g_killers[ply][0] == move){
        return;
    }
    g_killers[ply][1] = g_killers[ply][0];
    g_killer_used[ply][1] = g_killer_used[ply][0];
    g_killers[ply][0] = move;
    g_killer_used[ply][0] = true;
}

static int sq_index(const Point& p){
    return (int)p.first * BOARD_W + (int)p.second;
}

static int rep_adjusted_score(
    int score,
    int seen_count,
    int repetition_limit,
    int repetition_penalty,
    int attack_margin
){
    int occurrences_after_move = seen_count + 1;
    if(occurrences_after_move >= repetition_limit){
        return score > 0 ? 0 : score;
    }
    if(seen_count <= 0){
        return score;
    }

    int penalty = repetition_penalty * seen_count;
    if(score > attack_margin){
        return score - penalty;
    }
    if(score > 0){
        int adjusted = score - penalty / 2;
        return adjusted > 0 ? adjusted : 0;
    }
    return score - penalty / 4;
}

static int apply_rep_policy(
    int score,
    const State* child,
    const GameHistory& history,
    const MMParams& p
){
    return rep_adjusted_score(
        score,
        history.count(child->hash()),
        p.repetition_limit,
        p.repetition_penalty,
        p.repetition_attack_margin
    );
}

static int piece_value_at(const State* state, int player, int row, int col){
    if(row < 0 || row >= BOARD_H || col < 0 || col >= BOARD_W){
        return 0;
    }
    int piece = state->board.board[player][row][col];
    return piece ? PIECE_VALUES[piece] : 0;
}

static bool is_promotion_move(const State* state, const Move& move){
    int player = state->player;
    int fr = (int)move.first.first;
    int fc = (int)move.first.second;
    int tr = (int)move.second.first;
    if(fr < 0 || fr >= BOARD_H || fc < 0 || fc >= BOARD_W){
        return false;
    }
    int piece = state->board.board[player][fr][fc];
    return piece == 1 && (tr == 0 || tr == BOARD_H - 1);
}

static bool is_tactical_move(const State* state, const Move& move){
    int opp = 1 - state->player;
    int tr = (int)move.second.first;
    int tc = (int)move.second.second;
    if(tr < 0 || tr >= BOARD_H || tc < 0 || tc >= BOARD_W){
        return false;
    }
    return state->board.board[opp][tr][tc] || is_promotion_move(state, move);
}

static int move_order_score(const State* state, const Move& move){
    int player = state->player;
    int opp = 1 - player;
    int fr = (int)move.first.first;
    int fc = (int)move.first.second;
    int tr = (int)move.second.first;
    int tc = (int)move.second.second;

    int moving_type = 0;
    if(fr >= 0 && fr < BOARD_H && fc >= 0 && fc < BOARD_W){
        moving_type = state->board.board[player][fr][fc];
    }
    int moving_piece = piece_value_at(state, player, fr, fc);
    int captured_piece = piece_value_at(state, opp, tr, tc);
    int score = 0;
    if(captured_piece){
        score += 10000 + captured_piece * 16 - moving_piece;
        if(state->board.board[opp][tr][tc] == 6){
            score += 1000000;
        }
    }
    if(is_promotion_move(state, move)){
        score += 5000;
    }

    int enemy_kr = -1, enemy_kc = -1;
    for(int r = 0; r < BOARD_H; r++){
        for(int c = 0; c < BOARD_W; c++){
            if(state->board.board[opp][r][c] == 6){
                enemy_kr = r;
                enemy_kc = c;
            }
        }
    }
    if(enemy_kr >= 0 && tr >= 0 && tr < BOARD_H && tc >= 0 && tc < BOARD_W){
        int before = std::max(std::abs(fr - enemy_kr), std::abs(fc - enemy_kc));
        int after = std::max(std::abs(tr - enemy_kr), std::abs(tc - enemy_kc));
        int attack_weight = moving_type == 6 ? 45 : 180;
        if(after < before){
            score += (before - after) * attack_weight;
        }
        if(after <= 2){
            score += (3 - after) * (moving_type == 6 ? 80 : 320);
        }
        if(after <= 1 && moving_type != 6){
            score += 450;
        }
    }

    // light development bias for quiet root ordering
    if(tr >= 0 && tr < BOARD_H && tc >= 0 && tc < BOARD_W){
        int center_dist = std::abs(tr - (BOARD_H / 2)) + std::abs(tc - (BOARD_W / 2));
        score += 12 - center_dist;
    }
    return score;
}

static std::vector<Move> ordered_moves(State* state, int ply = 0, bool captures_only = false){
    Move tt_move = Move();
    bool has_tt_move = false;
    if(TTEntry* tte = tt_probe(state->hash())){
        tt_move = tte->best_move;
        has_tt_move = true;
    }

    std::vector<ScoredMove> scored;
    scored.reserve(state->legal_actions.size());
    for(const auto& move : state->legal_actions){
        if(captures_only && !is_tactical_move(state, move)){
            continue;
        }
        int score = move_order_score(state, move);
        if(has_tt_move && move == tt_move){
            score += 1000000;
        }
        if(!is_tactical_move(state, move)){
            if(killer_match(ply, move)){
                score += 8000;
            }
            int from = sq_index(move.first);
            int to = sq_index(move.second);
            if(from >= 0 && from < HIST_SQ && to >= 0 && to < HIST_SQ){
                score += g_history[state->player][from][to];
            }
        }
        scored.push_back({move, score});
    }
    std::stable_sort(scored.begin(), scored.end(), [](const ScoredMove& a, const ScoredMove& b){
        return a.score > b.score;
    });

    std::vector<Move> moves;
    moves.reserve(scored.size());
    for(const auto& item : scored){
        moves.push_back(item.move);
    }
    return moves;
}

static bool poll_stop(SearchContext& ctx){
    return (ctx.nodes & 1023ULL) == 0 && ctx.should_stop();
}

static int quiescence(
    State* state,
    int alpha,
    int beta,
    GameHistory& history,
    int ply,
    SearchContext& ctx,
    const MMParams& p,
    int qdepth
);

static int negamax(
    State* state,
    int depth,
    int alpha,
    int beta,
    GameHistory& history,
    int ply,
    SearchContext& ctx,
    const MMParams& p
){
    ctx.nodes++;
    if(ply > ctx.seldepth){
        ctx.seldepth = ply;
    }
    if(poll_stop(ctx)){
        return state->evaluate(p.use_kp_eval, p.use_eval_mobility, &history);
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

    uint64_t h = state->hash();
    int original_alpha = alpha;

    if(p.use_alpha_beta && depth > 0){
        TTEntry* tte = tt_probe(h);
        if(tte && tte->depth >= depth && ply > 0){
            if(tte->flag == TT_EXACT){
                return tte->score;
            }
            if(tte->flag == TT_LOWER && tte->score >= beta){
                return tte->score;
            }
            if(tte->flag == TT_UPPER && tte->score <= alpha){
                return tte->score;
            }
        }
    }

    if(depth <= 0){
        if(p.use_quiescence){
            return quiescence(
                state, alpha, beta, history, ply, ctx, p, p.quiescence_depth
            );
        }
        return state->evaluate(p.use_kp_eval, p.use_eval_mobility, &history);
    }

    history.push(h);

    int best_score = M_MAX;
    Move best_move = state->legal_actions.empty() ? Move() : state->legal_actions[0];
    bool first_child = true;
    int move_index = 0;
    std::vector<Move> moves = ordered_moves(state, ply, false);

    for(const auto& action : moves){
        State* next = state->next_state(action);
        next->get_legal_actions();
        bool same = next->same_player_as_parent();

        int score;
        if(!p.use_pvs || first_child){
            int child_alpha = p.use_alpha_beta ? alpha : M_MAX;
            int child_beta = p.use_alpha_beta ? beta : P_MAX;
            int raw = same
                ? negamax(next, depth - 1, child_alpha, child_beta, history, ply + 1, ctx, p)
                : negamax(next, depth - 1, -child_beta, -child_alpha, history, ply + 1, ctx, p);
            score = same ? raw : -raw;
        }else{
            bool quiet = !is_tactical_move(state, action);
            bool killer = killer_match(ply, action);
            int reduction = 0;
            if(quiet && !killer && move_index >= 3 && depth >= 3){
                reduction = std::min(depth - 1, std::max(1, depth / 3));
            }
            int raw = same
                ? negamax(next, depth - 1 - reduction, alpha, alpha + 1, history, ply + 1, ctx, p)
                : negamax(next, depth - 1 - reduction, -alpha - 1, -alpha, history, ply + 1, ctx, p);
            score = same ? raw : -raw;
            if(!ctx.stop && reduction > 0 && score > alpha){
                raw = same
                    ? negamax(next, depth - 1, alpha, alpha + 1, history, ply + 1, ctx, p)
                    : negamax(next, depth - 1, -alpha - 1, -alpha, history, ply + 1, ctx, p);
                score = same ? raw : -raw;
            }
            if(!ctx.stop && score > alpha && score < beta){
                raw = same
                    ? negamax(next, depth - 1, score, beta, history, ply + 1, ctx, p)
                    : negamax(next, depth - 1, -beta, -score, history, ply + 1, ctx, p);
                score = same ? raw : -raw;
            }
        }

        score = apply_rep_policy(score, next, history, p);

        delete next;

        if(ctx.stop){
            break;
        }
        first_child = false;
        move_index++;
        if(score > best_score){
            best_score = score;
            best_move = action;
        }
        if(score > alpha){
            alpha = score;
        }
        if(p.use_alpha_beta && alpha >= beta){
            if(!is_tactical_move(state, action)){
                killer_store(ply, action);
                int from = sq_index(action.first);
                int to = sq_index(action.second);
                if(from >= 0 && from < HIST_SQ && to >= 0 && to < HIST_SQ){
                    g_history[state->player][from][to] += depth * depth;
                }
            }
            tt_store(h, alpha, depth, TT_LOWER, action);
            history.pop(h);
            return alpha;
        }
    }

    history.pop(h);

    if(!ctx.stop && p.use_alpha_beta){
        TTFlag flag = best_score <= original_alpha ? TT_UPPER : TT_EXACT;
        tt_store(h, best_score, depth, flag, best_move);
    }

    if(best_score == M_MAX){
        return state->evaluate(p.use_kp_eval, p.use_eval_mobility, &history);
    }
    return best_score;
}

static int quiescence(
    State* state,
    int alpha,
    int beta,
    GameHistory& history,
    int ply,
    SearchContext& ctx,
    const MMParams& p,
    int qdepth
){
    ctx.nodes++;
    if(ply > ctx.seldepth){
        ctx.seldepth = ply;
    }
    if(poll_stop(ctx)){
        return state->evaluate(p.use_kp_eval, p.use_eval_mobility, &history);
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

    int stand_pat = state->evaluate(p.use_kp_eval, false, &history);
    if(stand_pat >= beta || qdepth <= 0){
        return stand_pat;
    }
    if(stand_pat > alpha){
        alpha = stand_pat;
    }

    std::vector<Move> moves = ordered_moves(state, ply, true);
    if(moves.empty()){
        return stand_pat;
    }

    uint64_t h = state->hash();
    history.push(h);

    for(const auto& action : moves){
        int tr = (int)action.second.first;
        int tc = (int)action.second.second;
        int victim = state->board.board[1 - state->player][tr][tc];
        static const int piece_delta[7] = {0, 100, 500, 320, 330, 900, 20000};
        if(victim > 0 && victim < 6 && stand_pat + piece_delta[victim] + 200 < alpha){
            continue;
        }

        State* next = state->next_state(action);
        next->get_legal_actions();
        bool same = next->same_player_as_parent();
        int raw = same
            ? quiescence(next, alpha, beta, history, ply + 1, ctx, p, qdepth - 1)
            : quiescence(next, -beta, -alpha, history, ply + 1, ctx, p, qdepth - 1);
        int score = same ? raw : -raw;
        score = apply_rep_policy(score, next, history, p);
        delete next;

        if(ctx.stop){
            break;
        }
        if(score >= beta){
            history.pop(h);
            return score;
        }
        if(score > alpha){
            alpha = score;
        }
    }

    history.pop(h);
    return alpha;
}


// MiniMax public compatibility wrapper
int MiniMax::eval_ctx(
    State *state,
    int depth,
    GameHistory& history,
    int ply,
    SearchContext& ctx,
    const MMParams& p
){
    return negamax(state, depth, M_MAX, P_MAX, history, ply, ctx, p);
}


// MiniMax search
SearchResult MiniMax::search(
    State *state,
    int depth,
    GameHistory& history,
    SearchContext& ctx
){
    auto t0 = std::chrono::steady_clock::now();
    ctx.reset();
    MMParams p = MMParams::from_map(ctx.params);
    if(depth <= 1){
        clear_search_tables();
    }
    SearchResult result;
    result.depth = depth;

    if(!state->legal_actions.size()){
        state->get_legal_actions();
    }

    if(state->legal_actions.empty()){
        result.best_move = Move();
        result.score = state->evaluate(p.use_kp_eval, p.use_eval_mobility, &history);
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

    std::vector<Move> moves = ordered_moves(state, 0, false);
    result.best_move = moves[0];
    result.pv = {result.best_move};

    int alpha = M_MAX;
    int beta = P_MAX;
    int best_score = M_MAX;
    int move_index = 0;
    int total_moves = (int)moves.size();

    for(const auto& action : moves){
        if(ctx.should_stop()){
            break;
        }

        State* next = state->next_state(action);
        next->get_legal_actions();
        bool same = next->same_player_as_parent();

        int raw;
        int score;
        if(!p.use_pvs || move_index == 0){
            int child_alpha = p.use_alpha_beta ? alpha : M_MAX;
            int child_beta = p.use_alpha_beta ? beta : P_MAX;
            raw = same
                ? negamax(next, depth - 1, child_alpha, child_beta, history, 1, ctx, p)
                : negamax(next, depth - 1, -child_beta, -child_alpha, history, 1, ctx, p);
            score = same ? raw : -raw;
        }else{
            raw = same
                ? negamax(next, depth - 1, alpha, alpha + 1, history, 1, ctx, p)
                : negamax(next, depth - 1, -alpha - 1, -alpha, history, 1, ctx, p);
            score = same ? raw : -raw;
            if(!ctx.stop && score > alpha && score < beta){
                raw = same
                    ? negamax(next, depth - 1, score, beta, history, 1, ctx, p)
                    : negamax(next, depth - 1, -beta, -score, history, 1, ctx, p);
                score = same ? raw : -raw;
            }
        }

        score = apply_rep_policy(score, next, history, p);
        delete next;

        if(ctx.stop){
            break;
        }

        if(score > best_score){
            best_score = score;
            result.best_move = action;
            result.score = score;
            result.pv = {action};
            if(p.report_partial && ctx.on_root_update){
                ctx.on_root_update({result.best_move, best_score, depth, move_index + 1, total_moves});
            }
        }
        if(score > alpha){
            alpha = score;
        }
        move_index++;
    }

    if(best_score == M_MAX){
        result.score = state->evaluate(p.use_kp_eval, p.use_eval_mobility, &history);
    }else{
        result.score = best_score;
    }
    result.depth = depth;
    result.seldepth = ctx.seldepth;
    result.nodes = ctx.nodes;
    result.time_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0
    ).count();
    if(!ctx.stop){
        tt_store(state->hash(), result.score, depth, TT_EXACT, result.best_move);
    }
    return result;
}


// MiniMax default_params / param_defs
ParamMap MiniMax::default_params(){
    return {
        {"UseKPEval", "true"},
        {"UseEvalMobility", "true"},
        {"ReportPartial", "false"},
        {"UseAlphaBeta", "true"},
        {"UsePVS", "true"},
        {"UseQuiescence", "true"},
        {"QuiescenceDepth", "4"},
        {"RepetitionLimit", "3"},
        {"RepetitionPenalty", "80"},
        {"RepetitionAttackMargin", "30"},
    };
}

std::vector<ParamDef> MiniMax::param_defs(){
    return {
        {"UseKPEval", ParamDef::CHECK, "true"},
        {"UseEvalMobility", ParamDef::CHECK, "true"},
        {"ReportPartial", ParamDef::CHECK, "false"},
        {"UseAlphaBeta", ParamDef::CHECK, "true"},
        {"UsePVS", ParamDef::CHECK, "true"},
        {"UseQuiescence", ParamDef::CHECK, "true"},
        {"QuiescenceDepth", ParamDef::SPIN, "4", 0, 8},
        {"RepetitionLimit", ParamDef::SPIN, "3", 2, 8},
        {"RepetitionPenalty", ParamDef::SPIN, "80", 0, 1000},
        {"RepetitionAttackMargin", ParamDef::SPIN, "30", 0, 10000},
    };
}
