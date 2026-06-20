#include <iostream>
#include <sstream>
#include <cstdint>
#include <cstdlib>
#include <algorithm>

#include "./state.hpp"
#include "config.hpp"
#include "../../policy/game_history.hpp"


// KP (King-Piece) Evaluation tables
// Always compiled; toggled at runtime via use_kp_eval param.

// KP material in centipawns. PST/tactical bonuses are secondary to material.
static const int kp_material[7] = {0, 100, 500, 320, 330, 900, 20000};

// Material-only (simple scale)
static const int simple_material[7] = {0, 1, 5, 3, 3, 9, 100};

// Piece-Square Tables (white perspective, mirror for black)
static const int pst[6][BOARD_H][BOARD_W] = {
    // Pawn
    {{ 0,  0,  0,  0,  0}, {50, 50, 50, 50, 50}, {10, 15, 20, 15, 10},
     { 5,  8, 12,  8,  5}, { 0,  3,  5,  3,  0}, { 0,  0,  0,  0,  0}},
    // Rook
    {{ 5,  5,  5,  5,  5}, {10, 10, 10, 10, 10}, { 0,  0,  3,  0,  0},
     { 0,  0,  3,  0,  0}, {-3,  0,  3,  0, -3}, { 0,  0,  0,  0,  0}},
    // Knight
    {{-20,-10,  0,-10,-20}, {-10,  5, 10,  5,-10}, {  0, 10, 15, 10,  0},
     {  0, 10, 15, 10,  0}, {-10,  5, 10,  5,-10}, {-20,-10,  0,-10,-20}},
    // Bishop
    {{-10,  0,  0,  0,-10}, {  0, 10, 12, 10,  0}, {  0, 12, 12, 12,  0},
     {  0, 12, 12, 12,  0}, {  0, 10, 12, 10,  0}, {-10,  0,  0,  0,-10}},
    // Queen
    {{-10, -5,  0, -5,-10}, { -5,  5, 10,  5, -5}, {  0, 10, 15, 10,  0},
     {  0, 10, 15, 10,  0}, { -5,  5, 10,  5, -5}, {-10, -5,  0, -5,-10}},
    // King
    {{-30,-30,-40,-30,-30}, {-20,-20,-30,-20,-20}, {-10,-10,-20,-10,-10},
     { -5, -5,-10, -5, -5}, { 10, 15,  0, 15, 10}, { 20, 25, 10, 25, 20}},
};

// king tropism weights — tuned for an attacking style
static const int tropism_w[7] = {0, 10, 18, 24, 20, 32, 0};

static int king_tropism(
    int piece_type,
    int pr, int pc,
    int ekr, int ekc
){
    int dist = std::max(std::abs(pr - ekr), std::abs(pc - ekc));
    if(dist <= 3){
        return tropism_w[piece_type] * (4 - dist);
    }
    return 0;
}

static bool occupied_any(const Board& board, int r, int c){
    return board.board[0][r][c] || board.board[1][r][c];
}

static bool clear_ray(const Board& board, int sr, int sc, int tr, int tc, int dr, int dc){
    int r = sr + dr;
    int c = sc + dc;
    while(r != tr || c != tc){
        if(r < 0 || r >= BOARD_H || c < 0 || c >= BOARD_W){
            return false;
        }
        if(occupied_any(board, r, c)){
            return false;
        }
        r += dr;
        c += dc;
    }
    return true;
}

static bool piece_attacks_square(
    const Board& board,
    int owner,
    int sr,
    int sc,
    int tr,
    int tc
){
    int piece = board.board[owner][sr][sc];
    if(!piece || (sr == tr && sc == tc)){
        return false;
    }

    int dr = tr - sr;
    int dc = tc - sc;
    int adr = std::abs(dr);
    int adc = std::abs(dc);

    switch(piece){
        case 1: {
            int forward = owner == 0 ? -1 : 1;
            return dr == forward && adc == 1;
        }
        case 2:
            if(dr == 0){
                return clear_ray(board, sr, sc, tr, tc, 0, dc > 0 ? 1 : -1);
            }
            if(dc == 0){
                return clear_ray(board, sr, sc, tr, tc, dr > 0 ? 1 : -1, 0);
            }
            return false;
        case 3:
            return (adr == 2 && adc == 1) || (adr == 1 && adc == 2);
        case 4:
            if(adr == adc){
                return clear_ray(board, sr, sc, tr, tc, dr > 0 ? 1 : -1, dc > 0 ? 1 : -1);
            }
            return false;
        case 5:
            if(dr == 0){
                return clear_ray(board, sr, sc, tr, tc, 0, dc > 0 ? 1 : -1);
            }
            if(dc == 0){
                return clear_ray(board, sr, sc, tr, tc, dr > 0 ? 1 : -1, 0);
            }
            if(adr == adc){
                return clear_ray(board, sr, sc, tr, tc, dr > 0 ? 1 : -1, dc > 0 ? 1 : -1);
            }
            return false;
        case 6:
            return std::max(adr, adc) == 1;
    }
    return false;
}

static int king_pressure_bonus(const Board& board, int attacker, int enemy_kr, int enemy_kc){
    if(enemy_kr < 0 || enemy_kc < 0){
        return 0;
    }

    int bonus = 0;
    for(int r = 0; r < BOARD_H; r++){
        for(int c = 0; c < BOARD_W; c++){
            int piece = board.board[attacker][r][c];
            if(!piece){
                continue;
            }
            int dist = std::max(std::abs(r - enemy_kr), std::abs(c - enemy_kc));
            if(dist <= 4){
                static const int chase_w[7] = {0, 10, 24, 30, 26, 40, 0};
                bonus += chase_w[piece] * (5 - dist);
            }
        }
    }

    int escape_squares = 0;
    int controlled_squares = 0;
    for(int dr = -1; dr <= 1; dr++){
        for(int dc = -1; dc <= 1; dc++){
            if(dr == 0 && dc == 0){
                continue;
            }
            int tr = enemy_kr + dr;
            int tc = enemy_kc + dc;
            if(tr < 0 || tr >= BOARD_H || tc < 0 || tc >= BOARD_W){
                bonus += 16;
                continue;
            }
            if(board.board[attacker][tr][tc]){
                bonus += 24;
                controlled_squares++;
                continue;
            }

            bool controlled = false;
            for(int r = 0; r < BOARD_H && !controlled; r++){
                for(int c = 0; c < BOARD_W; c++){
                    if(piece_attacks_square(board, attacker, r, c, tr, tc)){
                        controlled = true;
                        break;
                    }
                }
            }
            if(controlled){
                bonus += 20;
                controlled_squares++;
            }else if(!board.board[1 - attacker][tr][tc]){
                escape_squares++;
            }
        }
    }

    bonus += controlled_squares * 18;
    bonus -= escape_squares * 8;
    return bonus;
}

static const int king_endgame_pst[BOARD_H][BOARD_W] = {
    {-10, -5,  0, -5,-10},
    { -5, 10, 15, 10, -5},
    {  0, 15, 20, 15,  0},
    {  0, 15, 20, 15,  0},
    { -5, 10, 15, 10, -5},
    {-10, -5,  0, -5,-10},
};

static bool is_endgame_pos(const Board& board){
    int heavy = 0;
    for(int p = 0; p < 2; p++){
        for(int r = 0; r < BOARD_H; r++){
            for(int c = 0; c < BOARD_W; c++){
                int piece = board.board[p][r][c];
                if(piece == 2 || piece == 5){
                    heavy++;
                }
            }
        }
    }
    return heavy <= 1;
}

static bool is_passed_pawn(
    const char enemy_board[BOARD_H][BOARD_W],
    int owner,
    int r,
    int c
){
    int dir = owner == 0 ? -1 : 1;
    for(int ahead = r + dir; ahead >= 0 && ahead < BOARD_H; ahead += dir){
        for(int dc = -1; dc <= 1; dc++){
            int fc = c + dc;
            if(fc >= 0 && fc < BOARD_W && enemy_board[ahead][fc] == 1){
                return false;
            }
        }
    }
    return true;
}

static int passed_pawn_bonus(int owner, int r){
    int rows_left = owner == 0 ? r : (BOARD_H - 1 - r);
    return 22 + (BOARD_H - rows_left) * 17;
}

static int pawn_structure_bonus(const Board& board, int perspective_player){
    int bonus = 0;
    for(int pl = 0; pl < 2; pl++){
        int sign = (pl == perspective_player) ? 1 : -1;
        int pawn_cols[BOARD_W] = {};
        for(int r = 0; r < BOARD_H; r++){
            for(int c = 0; c < BOARD_W; c++){
                if(board.board[pl][r][c] == 1){
                    pawn_cols[c]++;
                }
            }
        }
        for(int c = 0; c < BOARD_W; c++){
            if(pawn_cols[c] == 0){
                continue;
            }
            if(pawn_cols[c] >= 2){
                bonus -= sign * 22 * (pawn_cols[c] - 1);
            }
            bool has_neighbor = (c > 0 && pawn_cols[c - 1] > 0)
                || (c < BOARD_W - 1 && pawn_cols[c + 1] > 0);
            if(!has_neighbor){
                bonus -= sign * 17;
            }
        }
    }
    return bonus;
}

static int rook_on_seventh_bonus(const Board& board, int perspective_player){
    int bonus = 0;
    for(int pl = 0; pl < 2; pl++){
        int sign = (pl == perspective_player) ? 1 : -1;
        int seventh_rank = pl == 0 ? 1 : BOARD_H - 2;
        for(int c = 0; c < BOARD_W; c++){
            if(board.board[pl][seventh_rank][c] == 2){
                bonus += sign * 44;
            }
        }
    }
    return bonus;
}

static int score_side(
    const char owning_board[BOARD_H][BOARD_W],
    const char enemy_board[BOARD_H][BOARD_W],
    int owner,
    bool endgame,
    int enemy_kr,
    int enemy_kc
){
    int score = 0;
    for(int r = 0; r < BOARD_H; r++){
        for(int c = 0; c < BOARD_W; c++){
            int piece = owning_board[r][c];
            if(!piece){
                continue;
            }

            int pst_row = owner == 0 ? r : BOARD_H - 1 - r;
            int pst_col = owner == 0 ? c : BOARD_W - 1 - c;
            score += kp_material[piece];
            if(piece == 6 && endgame){
                score += king_endgame_pst[pst_row][pst_col];
            }else{
                score += pst[piece - 1][pst_row][pst_col];
            }
            if(enemy_kr >= 0){
                score += king_tropism(piece, r, c, enemy_kr, enemy_kc);
            }
            if(piece == 1 && is_passed_pawn(enemy_board, owner, r, c)){
                score += passed_pawn_bonus(owner, r);
            }
        }
    }
    return score;
}


// evaluate() — runtime-selectable eval strategy

int State::evaluate(
    bool use_kp_eval,
    bool use_mobility,
    const GameHistory* history
){
    if(this->game_state == WIN){
        return P_MAX;
    }
    if(this->game_state == DRAW){
        return 0;
    }
    
    auto& self_board = this->board.board[this->player];
    auto& oppn_board = this->board.board[1 - this->player];
    int self_score = 0, oppn_score = 0;
    bool endgame = is_endgame_pos(this->board);
    int self_kr = -1, self_kc = -1;
    int oppn_kr = -1, oppn_kc = -1;

    if(use_kp_eval){
        // kp eval: material + PST + tropism

        for(int r = 0; r < BOARD_H; r++){
            for(int c = 0; c < BOARD_W; c++){
                if(self_board[r][c] == 6){
                    self_kr = r;
                    self_kc = c;
                }
                if(oppn_board[r][c] == 6){
                    oppn_kr = r;
                    oppn_kc = c;
                }
            }
        }

        self_score = score_side(
            self_board, oppn_board, this->player, endgame, oppn_kr, oppn_kc
        );
        oppn_score = score_side(
            oppn_board, self_board, 1 - this->player, endgame, self_kr, self_kc
        );

    }else{
        // simple material-only eval

        for(int r = 0; r < BOARD_H; r++){
            for(int c = 0; c < BOARD_W; c++){
                self_score += simple_material[(int)self_board[r][c]];
                oppn_score += simple_material[(int)oppn_board[r][c]];
            }
        }
    }

    int bonus = 0;

    // mobility bonus
    if(use_mobility){
        int self_mobility = (int)this->legal_actions.size();
        if(self_mobility == 0 && this->game_state == UNKNOWN){
            State self_state(this->board, this->player);
            self_state.get_legal_actions();
            self_mobility = (int)self_state.legal_actions.size();
        }
        State oppn_state(this->board, 1 - this->player);
        oppn_state.get_legal_actions();
        bonus += 4 * (self_mobility - (int)oppn_state.legal_actions.size());

    }

    if(use_kp_eval){
        bonus += rook_on_seventh_bonus(this->board, this->player);
        bonus += pawn_structure_bonus(this->board, this->player) / 2;
        bonus += king_pressure_bonus(this->board, this->player, oppn_kr, oppn_kc);
        bonus -= king_pressure_bonus(this->board, 1 - this->player, self_kr, self_kc) / 2;
    }

    if(history && history->count(this->hash()) >= 2){
        bonus -= 10;
    }

    return self_score - oppn_score + bonus;
}



// Zobrist hash for transposition table
static uint64_t zobrist_piece[2][7][BOARD_H][BOARD_W];
static uint64_t zobrist_side;
static bool zobrist_ready = false;

static void init_zobrist(){
    uint64_t s = 0x7A35C9D1E4F02B68ULL;
    auto rand64 = [&s]() -> uint64_t {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s;
    };
    for(int p = 0; p < 2; p++){
        for(int t = 0; t < 7; t++){
            for(int r = 0; r < BOARD_H; r++){
                for(int c = 0; c < BOARD_W; c++){
                    zobrist_piece[p][t][r][c] = rand64();
                }
            }
        }
    }
    zobrist_side = rand64();
    zobrist_ready = true;
}

uint64_t State::compute_hash_full() const{
    if(!zobrist_ready){
        init_zobrist();
    }
    uint64_t h = 0;
    for(int p = 0; p < 2; p++){
        for(int r = 0; r < BOARD_H; r++){
            for(int c = 0; c < BOARD_W; c++){
                int piece = this->board.board[p][r][c];
                if(piece){
                    h ^= zobrist_piece[p][piece][r][c];
                }
            }
        }
    }
    if(this->player){
        h ^= zobrist_side;
    }
    return h;
}


// returns the next State after applying the given move
State* State::next_state(const Move& move){
    if(!zobrist_ready){ init_zobrist(); }

    Board next = this->board;
    Point from = move.first, to = move.second;
    int p = this->player;
    int opp = 1 - p;

    int8_t orig_piece = next.board[p][from.first][from.second];
    int8_t moved = orig_piece;
    // pawn promotion
    if(moved == 1 && (to.first==BOARD_H-1 || to.first==0)){
        moved = 5;
    }

    // incremental Zobrist update
    uint64_t h = this->hash();
    h ^= zobrist_side;  // flip side-to-move bit

    // remove piece from source square
    h ^= zobrist_piece[p][orig_piece][from.first][from.second];

    // remove captured piece at destination
    int8_t captured = next.board[opp][to.first][to.second];
    if(captured){
        h ^= zobrist_piece[opp][captured][to.first][to.second];
        next.board[opp][to.first][to.second] = 0;
    }

    // place moved (or promoted) piece at destination
    h ^= zobrist_piece[p][moved][to.first][to.second];

    next.board[p][from.first][from.second] = 0;
    next.board[p][to.first][to.second] = moved;

    State* ns = new State(next, opp);
    ns->step = this->step + 1;
    ns->zobrist_hash = h;
    ns->zobrist_valid = true;
    return ns;
}


static const int move_table_rook_bishop[8][7][2] = {
  {{0, 1}, {0, 2}, {0, 3}, {0, 4}, {0, 5}, {0, 6}, {0, 7}},
  {{0, -1}, {0, -2}, {0, -3}, {0, -4}, {0, -5}, {0, -6}, {0, -7}},
  {{1, 0}, {2, 0}, {3, 0}, {4, 0}, {5, 0}, {6, 0}, {7, 0}},
  {{-1, 0}, {-2, 0}, {-3, 0}, {-4, 0}, {-5, 0}, {-6, 0}, {-7, 0}},
  {{1, 1}, {2, 2}, {3, 3}, {4, 4}, {5, 5}, {6, 6}, {7, 7}},
  {{1, -1}, {2, -2}, {3, -3}, {4, -4}, {5, -5}, {6, -6}, {7, -7}},
  {{-1, 1}, {-2, 2}, {-3, 3}, {-4, 4}, {-5, 5}, {-6, 6}, {-7, 7}},
  {{-1, -1}, {-2, -2}, {-3, -3}, {-4, -4}, {-5, -5}, {-6, -6}, {-7, -7}},
};

static const int move_table_knight[8][2] = {
  {1, 2}, {1, -2}, {-1, 2}, {-1, -2},
  {2, 1}, {2, -1}, {-2, 1}, {-2, -1},
};
static const int move_table_king[8][2] = {
  {1, 0}, {0, 1}, {-1, 0}, {0, -1}, 
  {1, 1}, {1, -1}, {-1, 1}, {-1, -1},
};


// naive move generation — array-based, branch-heavy
void State::get_legal_actions_naive(){
    this->game_state = NONE;
    std::vector<Move> all_actions;
    all_actions.reserve(64);
    auto self_board = this->board.board[this->player];
    auto oppn_board = this->board.board[1 - this->player];

    int now_piece, oppn_piece;
    for(int i=0; i<BOARD_H; i+=1){
        for(int j=0; j<BOARD_W; j+=1){
            if((now_piece=self_board[i][j])){
                switch(now_piece){
                    case 1: // pawn
                        if(this->player && i<BOARD_H-1){
                            // black
                            if(!oppn_board[i+1][j] && !self_board[i+1][j]){
                                all_actions.push_back(Move(Point(i, j), Point(i+1, j)));
                            }
                            if(j<BOARD_W-1 && (oppn_piece=oppn_board[i+1][j+1])>0){
                                all_actions.push_back(Move(Point(i, j), Point(i+1, j+1)));
                                if(oppn_piece==6){
                                    this->game_state = WIN;
                                    this->legal_actions = all_actions;
                                    return;
                                }
                            }
                            if(j>0 && (oppn_piece=oppn_board[i+1][j-1])>0){
                                all_actions.push_back(Move(Point(i, j), Point(i+1, j-1)));
                                if(oppn_piece==6){
                                    this->game_state = WIN;
                                    this->legal_actions = all_actions;
                                    return;
                                }
                            }
                        }else if(!this->player && i>0){
                            // white
                            if(!oppn_board[i-1][j] && !self_board[i-1][j]){
                                all_actions.push_back(Move(Point(i, j), Point(i-1, j)));
                            }
                            if(j<BOARD_W-1 && (oppn_piece=oppn_board[i-1][j+1])>0){
                                all_actions.push_back(Move(Point(i, j), Point(i-1, j+1)));
                                if(oppn_piece==6){
                                    this->game_state = WIN;
                                    this->legal_actions = all_actions;
                                    return;
                                }
                            }
                            if(j>0 && (oppn_piece=oppn_board[i-1][j-1])>0){
                                all_actions.push_back(Move(Point(i, j), Point(i-1, j-1)));
                                if(oppn_piece==6){
                                    this->game_state = WIN;
                                    this->legal_actions = all_actions;
                                    return;
                                }
                            }
                        }
                        break;

                    case 2: // rook
                    case 4: // bishop
                    case 5: // queen
                        int st, end;
                        switch(now_piece){
                            case 2: st=0; end=4; break; // rook
                            case 4: st=4; end=8; break; // bishop
                            case 5: st=0; end=8; break; // queen
                            default: st=0; end=-1;
                        }
                        for(int part=st; part<end; part+=1){
                            auto move_list = move_table_rook_bishop[part];
                            for(int k=0; k<std::max(BOARD_H, BOARD_W); k+=1){
                                int p[2] = {move_list[k][0] + i, move_list[k][1] + j};

                                if(p[0]>=BOARD_H || p[0]<0 || p[1]>=BOARD_W || p[1]<0){
                                    break;
                                }
                                now_piece = self_board[p[0]][p[1]];
                                if(now_piece){
                                    break;
                                }

                                all_actions.push_back(Move(Point(i, j), Point(p[0], p[1])));

                                oppn_piece = oppn_board[p[0]][p[1]];
                                if(oppn_piece){
                                    if(oppn_piece==6){
                                        this->game_state = WIN;
                                        this->legal_actions = all_actions;
                                        return;
                                    }else{
                                        break;
                                    }
                                };
                            }
                        }
                        break;

                    case 3: // knight
                        for(auto move: move_table_knight){
                            int p[2] = {move[0] + i, move[1] + j};

                            if(p[0]>=BOARD_H || p[0]<0 || p[1]>=BOARD_W || p[1]<0){
                                continue;
                            }
                            now_piece = self_board[p[0]][p[1]];
                            if(now_piece){
                                continue;
                            }

                            all_actions.push_back(Move(Point(i, j), Point(p[0], p[1])));

                            oppn_piece = oppn_board[p[0]][p[1]];
                            if(oppn_piece==6){
                                this->game_state = WIN;
                                this->legal_actions = all_actions;
                                return;
                            }
                        }
                        break;

                    case 6: // king
                        for(auto move: move_table_king){
                            int p[2] = {move[0] + i, move[1] + j};

                            if(p[0]>=BOARD_H || p[0]<0 || p[1]>=BOARD_W || p[1]<0){
                                continue;
                            }
                            now_piece = self_board[p[0]][p[1]];
                            if(now_piece){
                                continue;
                            }

                            all_actions.push_back(Move(Point(i, j), Point(p[0], p[1])));

                            oppn_piece = oppn_board[p[0]][p[1]];
                            if(oppn_piece==6){
                                this->game_state = WIN;
                                this->legal_actions = all_actions;
                                return;
                            }
                        }
                        break;
                }
            }
        }
    }
    this->legal_actions = all_actions;
}


// bitboard move generation
// 6x5 = 30 squares fit in a uint32_t; square (r,c) → bit index r*5+c.
// Precomputed attack masks for leapers (knight, king, pawn).
// Bit-scan via __builtin_ctz replaces nested array iteration.
#define BB_SQ(r, c)  ((r) * BOARD_W + (c))
#define BB_ROW(sq)   ((sq) / BOARD_W)
#define BB_COL(sq)   ((sq) % BOARD_W)

// Precomputed attack tables (initialized once)
static uint32_t bb_knight[30];       // knight attack mask per square
static uint32_t bb_king[30];         // king attack mask per square
static uint32_t bb_pawn_push[2][30]; // pawn push target per player/square
static uint32_t bb_pawn_cap[2][30];  // pawn capture targets per player/square
static bool bb_ready = false;

// Sliding piece direction vectors (0-3: rook, 4-7: bishop, 0-7: queen)
static const int bb_dr[8] = {0, 0, 1, -1, 1, 1, -1, -1};
static const int bb_dc[8] = {1, -1, 0, 0, 1, -1, 1, -1};

static void bb_init(){
    static const int kn_dr[8] = {1, 1, -1, -1, 2, 2, -2, -2};
    static const int kn_dc[8] = {2, -2, 2, -2, 1, -1, 1, -1};
    static const int ki_dr[8] = {1, 0, -1, 0, 1, 1, -1, -1};
    static const int ki_dc[8] = {0, 1, 0, -1, 1, -1, 1, -1};

    for(int r = 0; r < BOARD_H; r++){
        for(int c = 0; c < BOARD_W; c++){
            int sq = BB_SQ(r, c);

            // Knight
            bb_knight[sq] = 0;
            for(int d = 0; d < 8; d++){
                int nr = r + kn_dr[d], nc = c + kn_dc[d];
                if(nr >= 0 && nr < BOARD_H && nc >= 0 && nc < BOARD_W){
                    bb_knight[sq] |= 1u << BB_SQ(nr, nc);
                }
            }

            // King
            bb_king[sq] = 0;
            for(int d = 0; d < 8; d++){
                int nr = r + ki_dr[d], nc = c + ki_dc[d];
                if(nr >= 0 && nr < BOARD_H && nc >= 0 && nc < BOARD_W){
                    bb_king[sq] |= 1u << BB_SQ(nr, nc);
                }
            }

            // Pawn (player 0 = white, advances up = row-1)
            bb_pawn_push[0][sq] = 0;
            bb_pawn_cap[0][sq] = 0;
            if(r > 0){
                bb_pawn_push[0][sq] = 1u << BB_SQ(r-1, c);
                if(c > 0){
                    bb_pawn_cap[0][sq] |= 1u << BB_SQ(r-1, c-1);
                }
                if(c < BOARD_W-1){
                    bb_pawn_cap[0][sq] |= 1u << BB_SQ(r-1, c+1);
                }
            }

            // Pawn (player 1 = black, advances down = row+1)
            bb_pawn_push[1][sq] = 0;
            bb_pawn_cap[1][sq] = 0;
            if(r < BOARD_H-1){
                bb_pawn_push[1][sq] = 1u << BB_SQ(r+1, c);
                if(c > 0){
                    bb_pawn_cap[1][sq] |= 1u << BB_SQ(r+1, c-1);
                }
                if(c < BOARD_W-1){
                    bb_pawn_cap[1][sq] |= 1u << BB_SQ(r+1, c+1);
                }
            }
        }
    }
    bb_ready = true;
}

void State::get_legal_actions_bitboard(){
    if(!bb_ready){
        bb_init();
    }

    this->game_state = NONE;
    this->legal_actions.clear();
    this->legal_actions.reserve(64);

    int self = this->player;
    int oppn = 1 - self;

    // Build occupancy bitmasks and piece-type lookup
    uint32_t self_occ = 0, oppn_occ = 0;
    int self_pt[30] = {};  // piece type at each square (self)
    int oppn_pt[30] = {};  // piece type at each square (opponent)

    for(int r = 0; r < BOARD_H; r++){
        for(int c = 0; c < BOARD_W; c++){
            int sq = BB_SQ(r, c);
            if(this->board.board[self][r][c]){
                self_occ |= 1u << sq;
                self_pt[sq] = this->board.board[self][r][c];
            }
            if(this->board.board[oppn][r][c]){
                oppn_occ |= 1u << sq;
                oppn_pt[sq] = this->board.board[oppn][r][c];
            }
        }
    }

    uint32_t all_occ = self_occ | oppn_occ;

    // Iterate own pieces via bit scan
    uint32_t pieces = self_occ;
    while(pieces){
        int sq = __builtin_ctz(pieces);
        pieces &= pieces - 1;
        int r = BB_ROW(sq), c = BB_COL(sq);
        int piece = self_pt[sq];
        uint32_t targets = 0;

        switch(piece){
            case 1: { // Pawn
                uint32_t push = bb_pawn_push[self][sq] & ~all_occ;
                uint32_t cap = bb_pawn_cap[self][sq] & oppn_occ;
                // Check for king capture in captures
                uint32_t cap_scan = cap;
                while(cap_scan){
                    int to = __builtin_ctz(cap_scan);
                    cap_scan &= cap_scan - 1;
                    if(oppn_pt[to] == 6){
                        this->game_state = WIN;
                        this->legal_actions.push_back(
                            Move(Point(r, c), Point(BB_ROW(to), BB_COL(to))));
                        return;
                    }
                }
                targets = push | cap;
                break;
            }

            case 3: { // Knight
                targets = bb_knight[sq] & ~self_occ;
                uint32_t opp_targets = targets & oppn_occ;
                while(opp_targets){
                    int to = __builtin_ctz(opp_targets);
                    opp_targets &= opp_targets - 1;
                    if(oppn_pt[to] == 6){
                        this->game_state = WIN;
                        this->legal_actions.push_back(
                            Move(Point(r, c), Point(BB_ROW(to), BB_COL(to))));
                        return;
                    }
                }
                break;
            }

            case 6: { // King
                targets = bb_king[sq] & ~self_occ;
                uint32_t opp_targets = targets & oppn_occ;
                while(opp_targets){
                    int to = __builtin_ctz(opp_targets);
                    opp_targets &= opp_targets - 1;
                    if(oppn_pt[to] == 6){
                        this->game_state = WIN;
                        this->legal_actions.push_back(
                            Move(Point(r, c), Point(BB_ROW(to), BB_COL(to))));
                        return;
                    }
                }
                break;
            }

            case 2: // Rook
            case 4: // Bishop
            case 5: { // Queen
                int d_start = (piece == 4) ? 4 : 0;
                int d_end   = (piece == 2) ? 4 : 8;
                for(int d = d_start; d < d_end; d++){
                    int cr = r + bb_dr[d], cc = c + bb_dc[d];
                    while(cr >= 0 && cr < BOARD_H && cc >= 0 && cc < BOARD_W){
                        int to = BB_SQ(cr, cc);
                        uint32_t to_bit = 1u << to;
                        if(self_occ & to_bit){
                            break; // own piece blocks
                        }

                        if((oppn_occ & to_bit) && oppn_pt[to] == 6){
                            this->game_state = WIN;
                            this->legal_actions.push_back(
                                Move(Point(r, c), Point(cr, cc)));
                            return;
                        }

                        targets |= to_bit;
                        if(oppn_occ & to_bit){
                            break; // captured, stop sliding
                        }
                        cr += bb_dr[d]; cc += bb_dc[d];
                    }
                }
                break;
            }
        }

        // Convert target bitmask to Move objects
        while(targets){
            int to = __builtin_ctz(targets);
            targets &= targets - 1;
            this->legal_actions.push_back(
                Move(Point(r, c), Point(BB_ROW(to), BB_COL(to))));
        }
    }
}


// dispatcher — selects move-gen backend at compile time
void State::get_legal_actions(){
    #ifdef USE_BITBOARD
    get_legal_actions_bitboard();
    #else
    get_legal_actions_naive();
    #endif
}


/*
const char piece_table[2][7][5] = {
  {" ", "♙", "♖", "♘", "♗", "♕", "♔"},
  {" ", "♟", "♜", "♞", "♝", "♛", "♚"}
};
*/
const char piece_table[2][7][5] = {
  {" ", "P", "R", "N", "B", "Q", "K"},
  {" ", "p", "r", "n", "b", "q", "k"}
};
// encodes board to command-line display string
std::string State::encode_output() const{
    std::stringstream ss;
    int now_piece;
    for(int i=0; i<BOARD_H; i+=1){
        for(int j=0; j<BOARD_W; j+=1){
            if((now_piece = this->board.board[0][i][j])){
                ss << std::string(piece_table[0][now_piece]);
            }else if((now_piece = this->board.board[1][i][j])){
                ss << std::string(piece_table[1][now_piece]);
            }else{
                ss << " ";
            }
            ss << " ";
        }
        ss << "\n";
    }
    return ss.str();
}


// encodes full state to player protocol format
std::string State::encode_state(){
    std::stringstream ss;
    ss << this->player;
    ss << "\n";
    for(int pl=0; pl<2; pl+=1){
        for(int i=0; i<BOARD_H; i+=1){
            for(int j=0; j<BOARD_W; j+=1){
                ss << int(this->board.board[pl][i][j]);
                ss << " ";
            }
            ss << "\n";
        }
        ss << "\n";
    }
    return ss.str();
}


BaseState* State::create_null_state() const{
    State* s = new State(this->board, 1 - this->player);
    s->get_legal_actions();
    return s;
}


// board serialization
static const char* piece_chars = ".PRNBQK";
static const char* piece_chars_lower = ".prnbqk";

std::string State::encode_board() const{
    std::string s;
    for(int r = 0; r < BOARD_H; r++){
        if(r > 0){
            s += '/';
        }
        for(int c = 0; c < BOARD_W; c++){
            int w = board.board[0][r][c];
            int b = board.board[1][r][c];
            if(w > 0 && w <= 6){
                s += piece_chars[w];
            }else if(b > 0 && b <= 6){
                s += piece_chars_lower[b];
            }else{
                s += '.';
            }
        }
    }
    return s;
}

void State::decode_board(const std::string& s, int side_to_move){
    player = side_to_move;
    game_state = UNKNOWN;
    zobrist_valid = false;
    board = Board{};
    int r = 0, c = 0;
    for(char ch : s){
        if(ch == '/'){
            r++;
            c = 0;
            continue;
        }
        if(r >= BOARD_H || c >= BOARD_W){
            break;
        }
        if(ch >= 'A' && ch <= 'Z'){
            for(int p = 1; p <= 6; p++){
                if(piece_chars[p] == ch){
                    board.board[0][r][c] = p;
                    break;
                }
            }
        }else if(ch >= 'a' && ch <= 'z'){
            for(int p = 1; p <= 6; p++){
                if(piece_chars_lower[p] == ch){
                    board.board[1][r][c] = p;
                    break;
                }
            }
        }
        c++;
    }
    get_legal_actions();
}


// (Zobrist table declarations sit above next_state)


// cell display for protocol 'd' command
std::string State::cell_display(int row, int col) const{
    int w = static_cast<int>(board.board[0][row][col]);
    int b = static_cast<int>(board.board[1][row][col]);
    if(w){
        const char* names = ".PRNBQK";
        return std::string(" ") + names[w] + " ";
    }else if(b){
        const char* names = ".prnbqk";
        return std::string(" ") + names[b] + " ";
    }else{
        return " . ";
    }
}

// repetition check — chess 3-fold draw rule
bool State::check_repetition(const GameHistory& history, int& out_score) const {
    if(history.count(hash()) >= 3){
        out_score = 0;  // draw
        return true;
    }
    return false;
}
