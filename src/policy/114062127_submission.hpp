#pragma once
// Student search policy definition.
#include "search_types.hpp"
#include "game_history.hpp"

struct MMParams {
    bool use_kp_eval = true;
    bool use_eval_mobility = true;
    bool report_partial = false;
    bool use_alpha_beta = true;
    bool use_pvs = true;
    bool use_quiescence = true;
    int quiescence_depth = 4;
    int repetition_limit = 3;
    int repetition_penalty = 80;
    int repetition_attack_margin = 30;

    static MMParams from_map(const ParamMap& m){
        MMParams p;
        p.use_kp_eval       = param_bool(m, "UseKPEval", true);
        p.use_eval_mobility = param_bool(m, "UseEvalMobility", true);
        p.report_partial    = param_bool(m, "ReportPartial", false);
        p.use_alpha_beta    = param_bool(m, "UseAlphaBeta", true);
        p.use_pvs           = param_bool(m, "UsePVS", true);
        p.use_quiescence    = param_bool(m, "UseQuiescence", true);
        p.quiescence_depth  = param_int(m, "QuiescenceDepth", 4);
        p.repetition_limit  = param_int(m, "RepetitionLimit", 3);
        p.repetition_penalty = param_int(m, "RepetitionPenalty", 80);
        p.repetition_attack_margin = param_int(m, "RepetitionAttackMargin", 30);
        if(p.quiescence_depth < 0){
            p.quiescence_depth = 0;
        }
        if(p.quiescence_depth > 8){
            p.quiescence_depth = 8;
        }
        if(p.repetition_limit < 2){
            p.repetition_limit = 2;
        }
        if(p.repetition_penalty < 0){
            p.repetition_penalty = 0;
        }
        if(!p.use_alpha_beta){
            p.use_pvs = false;
        }
        return p;
    }
};

class MiniMax{
public:
    static int eval_ctx(
        State *state,
        int depth,
        GameHistory& history,
        int ply,
        SearchContext& ctx,
        const MMParams& p
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
