﻿/*
  RubiChess is a UCI chess playing engine by Andreas Matthies.

  RubiChess is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  RubiChess is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/


#include "RubiChess.h"

#ifdef STATISTICS
struct statistic statistics;
#endif

const int deltapruningmargin = 100;

int reductiontable[2][MAXDEPTH][64];

#define MAXLMPDEPTH 9
int lmptable[2][MAXLMPDEPTH];

// Shameless copy of Ethereal/Laser for now; may be improved/changed in the future
static const int SkipSize[16] = { 1, 1, 1, 2, 2, 2, 1, 3, 2, 2, 1, 3, 3, 2, 2, 1 };
static const int SkipDepths[16] = { 1, 2, 2, 4, 4, 3, 2, 5, 4, 3, 2, 6, 5, 4, 3, 2 };

void searchinit()
{
    for (int d = 0; d < MAXDEPTH; d++)
        for (int m = 0; m < 64; m++)
        {
            // reduction for not improving positions
            reductiontable[0][d][m] = 1 + (int)round(log(d * 1.5) * log(m) * 0.60);
            // reduction for improving positions
            reductiontable[1][d][m] = (int)round(log(d * 1.5) * log(m * 2) * 0.43);
        }
    for (int d = 0; d < MAXLMPDEPTH; d++)
    {
        // lmp for not improving positions
        lmptable[0][d] = (int)(2.5 + 0.7 * round(pow(d, 1.85)));
        // lmp for improving positions
        lmptable[1][d] = (int)(4.0 + 1.3 * round(pow(d, 1.85)));
    }
}

void chessposition::getCmptr(int16_t **cmptr)
{
    for (int i = 0, j = mstop - 1; i < CMPLIES; i++, j--)
    {
        uint32_t c;
        if (j >= 0 && (c = movestack[j].movecode))
            cmptr[i] = (int16_t*)counterhistory[GETPIECE(c)][GETTO(c)];
        else
            cmptr[i] = NULL;
    }
}

inline int chessposition::getHistory(uint32_t code, int16_t **cmptr)
{
    int pc = GETPIECE(code);
    int s2m = pc & S2MMASK;
    int from = GETFROM(code);
    int to = GETTO(code);
    int value = history[s2m][from][to];
    for (int i = 0; i < CMPLIES; i++)
        if (cmptr[i])
            value += cmptr[i][pc * 64 + to];

    return value;
}


inline void chessposition::updateHistory(uint32_t code, int16_t **cmptr, int value)
{
    int pc = GETPIECE(code);
    int s2m = pc & S2MMASK;
    int from = GETFROM(code);
    int to = GETTO(code);
    value = max(-256, min(256, value));
    int delta = 32 * value - history[s2m][from][to] * abs(value) / 256;
    history[s2m][from][to] += delta;
    for (int i = 0; i < CMPLIES; i++)
        if (cmptr[i]) {
            delta = 32 * value - cmptr[i][pc * 64 + to] * abs(value) / 256;
            cmptr[i][pc * 64 + to] += delta;
        }
}


int chessposition::getQuiescence(int alpha, int beta, int depth)
{
    int score;
    int bestscore = SHRT_MIN;
    bool myIsCheck = (bool)isCheckbb;
#ifdef EVALTUNE
    if (depth < 0) isQuiet = false;
    positiontuneset targetpts;
    evalparam ev[NUMOFEVALPARAMS];
    if (noQs)
    {
        // just evaluate and return (for tuning sets with just quiet positions)
        score = S2MSIGN(state & S2MMASK) * getEval<NOTRACE>();
        getPositionTuneSet(&targetpts, &ev[0]);
        copyPositionTuneSet(&targetpts, &ev[0], &this->pts, &this->ev[0]);
        return score;
    }

    bool foundpts = false;
#endif

    // FIXME: Should quiescience nodes count for the statistics?
    //en.nodes++;

    // Reset pv
    pvtable[ply][0] = 0;

#ifdef SDEBUG
    chessmove debugMove;
    int debugInsert = ply - rootheight;
    bool isDebugPv = triggerDebug(&debugMove);
#endif

    STATISTICSINC(qs_n[myIsCheck]);

    int hashscore = NOSCORE;
    uint16_t hashmovecode = 0;
    int staticeval = NOSCORE;
    bool tpHit = tp.probeHash(hash, &hashscore, &staticeval, &hashmovecode, depth, alpha, beta, ply);
    if (tpHit)
    {
        SDEBUGPRINT(isDebugPv, debugInsert, " Got score %d from TT.", hashscore);
        STATISTICSINC(qs_tt);
        return hashscore;
    }

    if (!myIsCheck)
    {
#ifdef EVALTUNE
        staticeval = S2MSIGN(state & S2MMASK) * getEval<NOTRACE>();
#else
        // get static evaluation of the position
        if (staticeval == NOSCORE)
        {
            if (movestack[mstop - 1].movecode == 0)
                staticeval = -staticevalstack[mstop - 1] + CEVAL(eps.eTempo, 2);
            else
                staticeval = S2MSIGN(state & S2MMASK) * getEval<NOTRACE>();
        }
#endif

        bestscore = staticeval;
        if (staticeval >= beta)
        {
            SDEBUGPRINT(isDebugPv, debugInsert, " Got score %d from qsearch (fail high by patscore).", staticeval);
            STATISTICSINC(qs_pat);
            return staticeval;
        }
        if (staticeval > alpha)
        {
#ifdef EVALTUNE
            getPositionTuneSet(&targetpts, &ev[0]);
            foundpts = true;
#endif
            alpha = staticeval;
        }

        // Delta pruning
        int bestCapture = getBestPossibleCapture();
        if (staticeval + deltapruningmargin + bestCapture < alpha)
        {
            SDEBUGPRINT(isDebugPv, debugInsert, " Got score %d from qsearch (delta pruning by patscore).", staticeval);
            STATISTICSINC(qs_delta);
            return staticeval;
        }
    }

    prepareStack();

    MoveSelector ms = {};
    ms.SetPreferredMoves(this);
    STATISTICSINC(qs_loop_n);

    chessmove *m;

    while ((m = ms.next()))
    {
        if (!myIsCheck && staticeval + materialvalue[GETCAPTURE(m->code) >> 1] + deltapruningmargin <= alpha)
        {
            // Leave out capture that is delta-pruned
            STATISTICSINC(qs_move_delta);
            continue;
        }

        if (!playMove(m))
            continue;

        STATISTICSINC(qs_moves);
        ms.legalmovenum++;
        score = -getQuiescence(-beta, -alpha, depth - 1);
        unplayMove(m);
        if (score > bestscore)
        {
            bestscore = score;
            if (score >= beta)
            {
                SDEBUGPRINT(isDebugPv, debugInsert, " Got score %d from qsearch (fail high).", score);
                STATISTICSINC(qs_moves_fh);
                return score;
            }
            if (score > alpha)
            {
                updatePvTable(m->code, true);
                alpha = score;
#ifdef EVALTUNE
                foundpts = true;
                copyPositionTuneSet(&this->pts, &this->ev[0], &targetpts, &ev[0]);
#endif
            }
        }
    }
#ifdef EVALTUNE
    if (foundpts)
        copyPositionTuneSet(&targetpts, &ev[0], &this->pts, &this->ev[0]);
#endif

    if (myIsCheck && !ms.legalmovenum)
    {
        // It's a mate
        SDEBUGPRINT(isDebugPv, debugInsert, " Got score %d from qsearch (mate).", SCOREBLACKWINS + ply);
        return SCOREBLACKWINS + ply;
    }

    SDEBUGPRINT(isDebugPv, debugInsert, " Got score %d from qsearch.", bestscore);
    return bestscore;
}



int chessposition::alphabeta(int alpha, int beta, int depth)
{
    int score;
    int hashscore = NOSCORE;
    uint16_t hashmovecode = 0;
    int staticeval = NOSCORE;
    int bestscore = NOSCORE;
    uint32_t bestcode = 0;
    int eval_type = HASHALPHA;
    chessmove *m;
    int extendall = 0;
    int effectiveDepth;
    bool PVNode = (alpha != beta - 1);

    nodes++;

    // Reset pv
    pvtable[ply][0] = 0;

#ifdef SDEBUG
    chessmove debugMove;
    string excludestr = "";
    int debugInsert = ply - rootheight;
    bool isDebugPv = triggerDebug(&debugMove);
#endif

    STATISTICSINC(ab_n);
    STATISTICSADD(ab_pv, PVNode);

    // test for remis via repetition
    int rep = testRepetiton();
    if (rep >= 2)
    {
        SDEBUGPRINT(isDebugPv, debugInsert, "Draw (repetition)");
        STATISTICSINC(ab_draw_or_win);
        return SCOREDRAW;
    }

    // test for remis via 50 moves rule
    if (halfmovescounter >= 100)
    {
        STATISTICSINC(ab_draw_or_win);
        if (!isCheckbb)
        {
            SDEBUGPRINT(isDebugPv, debugInsert, "Draw (50 moves)");
            return SCOREDRAW;
        } else {
            // special case: test for checkmate
            chessmovelist evasions;
            if (CreateMovelist<EVASION>(this, &evasions.move[0]) > 0)
                return SCOREDRAW;
            else
                return SCOREBLACKWINS + ply;
        }
    }

    if (en.stopLevel == ENGINESTOPIMMEDIATELY)
    {
        // time is over; immediate stop requested
        return beta;
    }

    // Reached depth? Do a qsearch
    if (depth <= 0)
    {
        // update selective depth info
        if (seldepth < ply + 1)
            seldepth = ply + 1;

        STATISTICSINC(ab_qs);
        return getQuiescence(alpha, beta, depth);
    }


    // Get move for singularity check and change hash to seperate partial searches from full searches
    uint16_t excludeMove = excludemovestack[mstop - 1];
    excludemovestack[mstop] = 0;

#ifdef SDEBUG
    if (isDebugPv)
    {
        chessmove cm, em;
        string s;
        for (int i = rootheight; i < mstop; i++)
        {
            cm.code = movestack[i].movecode;
            s = s + cm.toString() + " ";
        }
        if (excludeMove)
        {
            em.code = excludeMove;
            excludestr = " singular testing " + em.toString();
        }
        SDEBUGPRINT(true, debugInsert, "(depth=%2d%s) Entering debug pv: %s (%s)  [%3d,%3d] ", depth, excludestr.c_str(), s.c_str(), debugMove.code ? debugMove.toString().c_str() : "", alpha, beta);
    }
#endif

    U64 newhash = hash ^ excludeMove;

    bool tpHit = tp.probeHash(newhash, &hashscore, &staticeval, &hashmovecode, depth, alpha, beta, ply);
    if (tpHit)
    {
        if (!rep)
        {
            // not a single repetition; we can (almost) safely trust the hash value
            uint32_t fullhashmove = shortMove2FullMove(hashmovecode);
            if (fullhashmove)
                updatePvTable(fullhashmove, false);
            SDEBUGPRINT(isDebugPv, debugInsert, " Got score %d from TT.", hashscore);
            STATISTICSINC(ab_tt);
            return hashscore;
        }
    }


    // TB
    // The test for rule50_count() == 0 is required to prevent probing in case
    // the root position is a TB position but only WDL tables are available.
    // In that case the search should not probe before a pawn move or capture
    // is made.
    if (POPCOUNT(occupied00[0] | occupied00[1]) <= useTb && halfmovescounter == 0)
    {
        int success;
        int v = probe_wdl(&success, this);
        if (success) {
            en.tbhits++;
            int bound;
            if (v <= -1 - en.Syzygy50MoveRule) {
                bound = HASHALPHA;
                score = -SCORETBWIN + ply;
            }
            else if (v >= 1 + en.Syzygy50MoveRule) {
                bound = HASHBETA;
                score = SCORETBWIN - ply;
            }
            else {
                bound = HASHEXACT;
                score = SCOREDRAW + v;
            }
            if (bound == HASHEXACT || (bound == HASHALPHA ? (score <= alpha) : (score >= beta)))
            {
                tp.addHash(hash, score, staticeval, bound, MAXDEPTH, 0);
                SDEBUGPRINT(isDebugPv, debugInsert, " Got score %d from TB.", score);
            }
            STATISTICSINC(ab_tb);
            return score;
        }
    }

    // Check extension
    if (isCheckbb)
        extendall = 1;

    prepareStack();

    // get static evaluation of the position
    if (staticeval == NOSCORE)
    {
        if (movestack[mstop - 1].movecode == 0)
            // just reverse the staticeval before the null move respecting the tempo
            staticeval = -staticevalstack[mstop - 1] + CEVAL(eps.eTempo, 2);
        else
            staticeval = S2MSIGN(state & S2MMASK) * getEval<NOTRACE>();
    }
    staticevalstack[mstop] = staticeval;

    bool positionImproved = (mstop >= rootheight + 2
        && staticevalstack[mstop] > staticevalstack[mstop - 2]);

    // Razoring
    if (!PVNode && !isCheckbb && depth <= 2)
    {
        const int ralpha = alpha - 250 - depth * 50;
        if (staticeval < ralpha)
        {
            if (depth == 1 && ralpha < alpha)
                return getQuiescence(alpha, beta, depth);
            int value = getQuiescence(ralpha, ralpha + 1, depth);
            if (value <= ralpha)
                return value;
        }
    }

    // futility pruning
    bool futility = false;
    if (depth <= 6)
    {
        // reverse futility pruning
        if (!isCheckbb && staticeval - depth * (72 - 20 * positionImproved) > beta)
        {
            SDEBUGPRINT(isDebugPv, debugInsert, " Cutoff by reverse futility pruning: staticscore(%d) - revMargin(%d) > beta(%d)", staticeval, depth * (72 - 20 * positionImproved), beta);
            STATISTICSINC(prune_futility);
            return staticeval;
        }
        futility = (staticeval < alpha - (100 + 80 * depth));
    }

    // Nullmove pruning with verification like SF does it
    int bestknownscore = (hashscore != NOSCORE ? hashscore : staticeval);
    if (!isCheckbb && depth >= 2 && bestknownscore >= beta && (ply  >= nullmoveply || ply % 2 != nullmoveside))
    {
        playNullMove();
        int R = 4 + (depth / 6) + (bestknownscore - beta) / 150 + !PVNode * 2;

        score = -alphabeta(-beta, -beta + 1, depth - R);
        unplayNullMove();

        if (score >= beta)
        {
            if (MATEFORME(score))
                score = beta;

            if (abs(beta) < 5000 && (depth < 12 || nullmoveply)) {
                SDEBUGPRINT(isDebugPv, debugInsert, "Low-depth-cutoff by null move: %d", score);
                STATISTICSINC(prune_nm);
                return score;
            }
            // Verification search
            nullmoveply = ply + 3 * (depth - R) / 4;
            nullmoveside = ply % 2;
            int verificationscore = alphabeta(beta - 1, beta, depth - R);
            nullmoveside = nullmoveply = 0;
            if (verificationscore >= beta) {
                SDEBUGPRINT(isDebugPv, debugInsert, "Verified cutoff by null move: %d", score);
                STATISTICSINC(prune_nm);
                return score;
            }
            else {
                SDEBUGPRINT(isDebugPv, debugInsert, "Verification refutes cutoff by null move: %d", score);
            }
        }
    }

    // ProbCut
    if (!PVNode && depth >= 5 && abs(beta) < SCOREWHITEWINS)
    {
        int rbeta = min(SCOREWHITEWINS, beta + 100);
        chessmovelist *movelist = new chessmovelist;
        movelist->length = getMoves(&movelist->move[0], TACTICAL);

        for (int i = 0; i < movelist->length; i++)
        {
            if (!see(movelist->move[i].code, rbeta - staticeval))
                continue;

            if (playMove(&movelist->move[i]))
            {
                int probcutscore = -alphabeta(-rbeta, -rbeta + 1, depth - 4);

                unplayMove(&movelist->move[i]);

                if (probcutscore >= rbeta)
                {
                    // ProbCut off
                    delete movelist;
                    STATISTICSINC(prune_probcut);
                    return probcutscore;
                }
            }
        }
        delete movelist;
    }


    // Internal iterative deepening 
    const int iidmin = 3;
    const int iiddelta = 2;
    if (PVNode && !hashmovecode && depth >= iidmin)
    {
        SDEBUGPRINT(isDebugPv, debugInsert, " Entering iid...");
        alphabeta(alpha, beta, depth - iiddelta);
        hashmovecode = tp.getMoveCode(newhash);
    }

    // Get possible countermove from table
    uint32_t lastmove = movestack[mstop - 1].movecode;
    uint32_t counter = 0;
    if (lastmove)
        counter = countermove[GETPIECE(lastmove)][GETTO(lastmove)];

    // Reset killers for child ply
    killer[ply + 1][0] = killer[ply + 1][1] = 0;

    MoveSelector ms = {};
    ms.SetPreferredMoves(this, hashmovecode, killer[ply][0], killer[ply][1], counter, excludeMove);
    STATISTICSINC(moves_loop_n);

    int  LegalMoves = 0;
    int quietsPlayed = 0;
    uint32_t quietMoves[MAXMOVELISTLENGTH];
    while ((m = ms.next()))
    {
#ifdef SDEBUG
        bool isDebugMove = ((debugMove.code & 0xeff) == (m->code & 0xeff));
#endif
        STATISTICSINC(moves_n[(bool)ISTACTICAL(m->code)]);
        // Leave out the move to test for singularity
        if ((m->code & 0xffff) == excludeMove)
            continue;

        // Late move pruning
        if (depth < MAXLMPDEPTH && !ISTACTICAL(m->code) && bestscore > NOSCORE && quietsPlayed > lmptable[positionImproved][depth])
        {
            // Proceed to next moveselector state manually to save some time
            ms.state++;
            STATISTICSINC(moves_pruned_lmp);
            continue;
        }

        // Check for futility pruning condition for this move and skip move if at least one legal move is already found
        bool futilityPrune = futility && !ISTACTICAL(m->code) && !isCheckbb && alpha <= 900 && !moveGivesCheck(m->code);
        if (futilityPrune)
        {
            if (LegalMoves)
            {
                SDEBUGPRINT(isDebugPv && isDebugMove, debugInsert, " PV move %s pruned by futility: staticeval(%d) < alpha(%d) - futilityMargin(%d)", debugMove.toString().c_str(), staticeval, alpha, 100 + 80 * depth);
                STATISTICSINC(moves_pruned_futility);
                continue;
            }
            else if (staticeval > bestscore)
            {
                // Use the static score from futility test as a bestscore start value
                bestscore = staticeval;
            }
        }

        // Prune tactical moves with bad SEE
        if (!isCheckbb && depth < 8 && bestscore > NOSCORE && ms.state >= BADTACTICALSTATE && !see(m->code, -20 * depth * depth))
        {
            SDEBUGPRINT(isDebugPv && isDebugMove, debugInsert, " PV move %s pruned by bad SEE", debugMove.toString().c_str());
            STATISTICSINC(moves_pruned_badsee);
            continue;
        }

        int stats = getHistory(m->code, ms.cmptr);
        int extendMove = 0;

        // Singular extension
        if ((m->code & 0xffff) == hashmovecode
            && depth > 7
            && !excludeMove
            && tp.probeHash(newhash, &hashscore, &staticeval, &hashmovecode, depth - 3, alpha, beta, ply)  // FIXME: maybe needs hashscore = FIXMATESCOREPROBE(hashscore, ply);
            && hashscore > alpha)
        {
            SDEBUGPRINT(isDebugPv && isDebugMove, debugInsert, " PV move %s tested for singularity", debugMove.toString().c_str());
            excludemovestack[mstop - 1] = hashmovecode;
            int sBeta = max(hashscore - 2 * depth, SCOREBLACKWINS);
            int redScore = alphabeta(sBeta - 1, sBeta, depth / 2);
            excludemovestack[mstop - 1] = 0;

            if (redScore < sBeta)
            {
                // Move is singular
                SDEBUGPRINT(isDebugPv && isDebugMove, debugInsert, " PV move %s is singular", debugMove.toString().c_str());
                STATISTICSINC(extend_singular);
                extendMove = 1;
            }
            else if (bestknownscore >= beta && sBeta >= beta)
            {
                // Hashscore for lower depth and static eval cut and we have at least a second good move => lets cut here
                STATISTICSINC(prune_multicut);
                return sBeta;
            }
        }

        int reduction = 0;

        // Late move reduction
        if (depth > 2 && !ISTACTICAL(m->code))
        {
            reduction = reductiontable[positionImproved][depth][min(63, LegalMoves + 1)];

            // adjust reduction by stats value
            reduction -= stats / 4096;

            // adjust reduction at PV nodes
            reduction -= PVNode;

            STATISTICSINC(red_pi[positionImproved]);
            STATISTICSADD(red_lmr[positionImproved], reductiontable[positionImproved][depth][min(63, LegalMoves + 1)]);
            STATISTICSADD(red_history, -stats / 4096);
            STATISTICSADD(red_pv, -(int)PVNode);
            STATISTICSDO(int red0 = reduction);

            reduction = min(depth, max(0, reduction));

            STATISTICSDO(int red1 = reduction);
            STATISTICSADD(red_correction, red1 - red0);
            STATISTICSADD(red_total, reduction);

            SDEBUGPRINT(isDebugPv && isDebugMove && reduction, debugInsert, " PV move %s (value=%d) with depth reduced by %d", debugMove.toString().c_str(), m->value, reduction);
        }

        int pc = GETPIECE(m->code);
        int to = GETTO(m->code);
        effectiveDepth = depth + extendall - reduction + extendMove;

        // Prune moves with bad counter move history
        if (!ISTACTICAL(m->code) && effectiveDepth < 4
            && ms.cmptr[0] && ms.cmptr[0][pc * 64 + to] < 0
            && ms.cmptr[1] && ms.cmptr[1][pc * 64 + to] < 0)
            continue;

        if (!playMove(m))
            continue;

        LegalMoves++;

        // Check again for futility pruning now that we found a valid move
        if (futilityPrune)
        {
            SDEBUGPRINT(isDebugPv && isDebugMove, debugInsert, " PV move %s pruned by futility: staticeval(%d) < alpha(%d) - futilityMargin(%d)", debugMove.toString().c_str(), staticeval, alpha, 100 + 80 * depth);
            unplayMove(m);
            continue;
        }

        STATISTICSINC(moves_played[(bool)ISTACTICAL(m->code)]);

        if (eval_type != HASHEXACT)
        {
            // First move ("PV-move"); do a normal search
            score = -alphabeta(-beta, -alpha, effectiveDepth - 1);
            if (reduction && score > alpha)
            {
                // research without reduction
                effectiveDepth += reduction;
                score = -alphabeta(-beta, -alpha, effectiveDepth - 1);
            }
        }
        else {
            // try a PV-Search
            score = -alphabeta(-alpha - 1, -alpha, effectiveDepth - 1);
            if (score > alpha && score < beta)
            {
                // reasearch with full window
                score = -alphabeta(-beta, -alpha, effectiveDepth - 1);
            }
        }
        unplayMove(m);

        if (en.stopLevel == ENGINESTOPIMMEDIATELY)
        {
            // time is over; immediate stop requested
            return beta;
        }

        SDEBUGPRINT(isDebugPv && isDebugMove, debugInsert, " PV move %s scored %d", debugMove.toString().c_str(), score);

        if (score > bestscore)
        {
            bestscore = score;
            bestcode = m->code;

            if (score >= beta)
            {
                if (!ISTACTICAL(m->code))
                {
                    updateHistory(m->code, ms.cmptr, depth * depth);
                    for (int i = 0; i < quietsPlayed; i++)
                    {
                        uint32_t qm = quietMoves[i];
                        updateHistory(qm, ms.cmptr, -(depth * depth));
                    }

                    // Killermove
                    if (killer[ply][0] != m->code)
                    {
                        killer[ply][1] = killer[ply][0];
                        killer[ply][0] = m->code;
                    }

                    // save countermove
                    if (lastmove)
                        countermove[GETPIECE(lastmove)][GETTO(lastmove)] = m->code;
                }

                SDEBUGPRINT(isDebugPv, debugInsert, " Beta-cutoff by move %s: %d  %s%s", m->toString().c_str(), score, excludestr.c_str(), excludeMove ? " : not singular" : "");
                STATISTICSINC(moves_fail_high);

                if (!excludeMove)
                {
                    SDEBUGPRINT(isDebugPv, debugInsert, " ->Hash(%d) = %d(beta)", effectiveDepth, score);
                    tp.addHash(newhash, FIXMATESCOREADD(score, ply), staticeval, HASHBETA, effectiveDepth, (uint16_t)bestcode);
                }
                return score;   // fail soft beta-cutoff
            }

            if (score > alpha)
            {
                SDEBUGPRINT(isDebugPv && isDebugMove, debugInsert, " PV move %s raising alpha to %d", debugMove.toString().c_str(), score);
                alpha = score;
                eval_type = HASHEXACT;
                updatePvTable(bestcode, true);
            }
        }

        if (!ISTACTICAL(m->code))
            quietMoves[quietsPlayed++] = m->code;
    }

    if (LegalMoves == 0)
    {
        if (excludeMove)
            return alpha;

        STATISTICSINC(ab_draw_or_win);
        if (isCheckbb) {
            // It's a mate
            SDEBUGPRINT(isDebugPv, debugInsert, " Return score: %d  (mate)", SCOREBLACKWINS + ply);
            return SCOREBLACKWINS + ply;
        }
        else {
            // It's a stalemate
            SDEBUGPRINT(isDebugPv, debugInsert, " Return score: 0  (stalemate)");
            return SCOREDRAW;
        }
    }

    SDEBUGPRINT(isDebugPv, debugInsert, " Return score: %d  %s%s", bestscore, excludestr.c_str(), excludeMove ? " singular" : "");

    if (bestcode && !excludeMove)
    {
        SDEBUGPRINT(isDebugPv, debugInsert, " ->Hash(%d) = %d(%s)", depth, bestscore, eval_type == HASHEXACT ? "exact" : "alpha");
        tp.addHash(newhash, FIXMATESCOREADD(bestscore, ply), staticeval, eval_type, depth, (uint16_t)bestcode);
    }

    return bestscore;
}



template <RootsearchType RT>
int chessposition::rootsearch(int alpha, int beta, int depth)
{
    int score;
    uint16_t hashmovecode = 0;
    int bestscore = NOSCORE;
    int staticeval = NOSCORE;
    int eval_type = HASHALPHA;
    chessmove *m;
    int extendall = 0;
    int lastmoveindex;
    int maxmoveindex;

    const bool isMultiPV = (RT == MultiPVSearch);
    const bool doPonder = (RT == PonderSearch);

    nodes++;

    // reset pv
    pvtable[0][0] = 0;

    if (isMultiPV)
    {
        lastmoveindex = 0;
        maxmoveindex = min(en.MultiPV, rootmovelist.length);
        for (int i = 0; i < maxmoveindex; i++)
        {
            multipvtable[i][0] = 0;
            bestmovescore[i] = SHRT_MIN + 1;
        }
    }

#ifdef SDEBUG
    chessmove debugMove;
    int debugInsert = ply - rootheight;
    bool isDebugPv = triggerDebug(&debugMove);
    SDEBUGPRINT(true, debugInsert, "(depth=%2d) Rootsearch Next pv debug move: %s  [%3d,%3d]", depth, debugMove.code ? debugMove.toString().c_str() : "", alpha, beta);
#endif

    if (!isMultiPV
        && !useRootmoveScore
        && tp.probeHash(hash, &score, &staticeval, &hashmovecode, depth, alpha, beta, 0))
    {
        if (!testRepetiton())
        {
            // Not a single repetition so we trust the hash value but in some very rare cases it could happen that
            // a. the hashmove triggers 3-fold directly
            // b. the hashmove allows the opponent to get a 3-fold
            // see rep.txt in the test folder for examples
            // maybe this could be fixed in the future by using cuckoo tables like SF does it
            // https://marcelk.net/2013-04-06/paper/upcoming-rep-v2.pdf
            uint32_t fullhashmove = shortMove2FullMove(hashmovecode);
            if (fullhashmove)
            {
                if (bestmove.code != fullhashmove) {
                    bestmove.code = fullhashmove;
                    if (doPonder) pondermove.code = 0;
                }
                updatePvTable(fullhashmove, false);
                if (score > alpha) bestmovescore[0] = score;
                return score;
            }
        }
    }
    if (isCheckbb)
        extendall = 1;

    if (!tbPosition)
    {
        // Reset move values
        for (int i = 0; i < rootmovelist.length; i++)
        {
            m = &rootmovelist.move[i];

            //PV moves gets top score
            if (hashmovecode == (m->code & 0xffff))
                m->value = PVVAL;
            else if (bestFailingLow == m->code)
                m->value = KILLERVAL2 - 1;
            // killermoves gets score better than non-capture
            else if (killer[0][0] == m->code)
                m->value = KILLERVAL1;
            else if (killer[0][1] == m->code)
                m->value = KILLERVAL2;
            else if (GETCAPTURE(m->code) != BLANK)
                m->value = (mvv[GETCAPTURE(m->code) >> 1] | lva[GETPIECE(m->code) >> 1]);
            else 
                m->value = history[state & S2MMASK][GETFROM(m->code)][GETTO(m->code)];
        }
    }

    // get static evaluation of the position
    if (staticeval == NOSCORE)
        staticeval = S2MSIGN(state & S2MMASK) * getEval<NOTRACE>();
    staticevalstack[mstop] = staticeval;

    int quietsPlayed = 0;
    uint32_t quietMoves[MAXMOVELISTLENGTH];

    // FIXME: Dummy move selector for now; only used to pass null cmptr to updateHistory
    MoveSelector ms = {};

    for (int i = 0; i < rootmovelist.length; i++)
    {
        for (int j = i + 1; j < rootmovelist.length; j++)
            if (rootmovelist.move[i] < rootmovelist.move[j])
                swap(rootmovelist.move[i], rootmovelist.move[j]);

        m = &rootmovelist.move[i];
#ifdef SDEBUG
        bool isDebugMove = (debugMove.code == (m->code & 0xeff));
#endif

        playMove(m);

        if (en.moveoutput && !threadindex)
        {
            char s[256];
            sprintf_s(s, "info depth %d currmove %s currmovenumber %d\n", depth, m->toString().c_str(), i + 1);
            cout << s;
        }

        int reduction = 0;

        // Late move reduction
        if (!extendall && depth > 2 && !ISTACTICAL(m->code))
        {
            reduction = reductiontable[0][depth][min(63, i + 1)];
        }

        int effectiveDepth;
        if (eval_type != HASHEXACT)
        {
            // First move ("PV-move"); do a normal search
            effectiveDepth = depth + extendall - reduction;
            score = -alphabeta(-beta, -alpha, effectiveDepth - 1);
            if (reduction && score > alpha)
            {
                // research without reduction
                effectiveDepth += reduction;
                score = -alphabeta(-beta, -alpha, effectiveDepth - 1);
            }
        }
        else {
            // try a PV-Search
            effectiveDepth = depth + extendall;
            score = -alphabeta(-alpha - 1, -alpha, effectiveDepth - 1);
            if (score > alpha && score < beta)
            {
                // reasearch with full window
                score = -alphabeta(-beta, -alpha, effectiveDepth - 1);
            }
        }

        SDEBUGPRINT(isDebugPv && isDebugMove, debugInsert, " PV move %s scored %d", debugMove.toString().c_str(), score);

        unplayMove(m);

        if (en.stopLevel == ENGINESTOPIMMEDIATELY)
        {
            // time over; immediate stop requested
            return bestscore;
        }

        if (!ISTACTICAL(m->code))
            quietMoves[quietsPlayed++] = m->code;

        if ((isMultiPV && score <= bestmovescore[lastmoveindex])
            || (!isMultiPV && score <= bestscore))
            continue;

        bestscore = score;
        bestFailingLow = m->code;

        if (isMultiPV)
        {
            if (score > bestmovescore[lastmoveindex])
            {
                int newindex = lastmoveindex;
                while (newindex > 0 && score > bestmovescore[newindex - 1])
                {
                    bestmovescore[newindex] = bestmovescore[newindex - 1];
                    uint32_t *srctable = (newindex - 1 ? multipvtable[newindex - 1] : pvtable[0]);
                    memcpy(multipvtable[newindex], srctable, sizeof(multipvtable[newindex]));
                    newindex--;
                }
                updateMultiPvTable(newindex, m->code, true);

                bestmovescore[newindex] = score;
                if (lastmoveindex < maxmoveindex - 1)
                    lastmoveindex++;
                if (bestmovescore[maxmoveindex - 1] > alpha)
                {
                    alpha = bestmovescore[maxmoveindex - 1];
                }
                eval_type = HASHEXACT;
            }
        }

        // We have a new best move.
        // Now it gets a little tricky what to do with it
        // The move becomes the new bestmove (take for UCI output) if
        // - it is the first one
        // - it raises alpha
        // If it fails low we don't change bestmove anymore but remember it in bestFailingLow for move ordering
        if (score > alpha)
        {
            if (!isMultiPV)
            {
                updatePvTable(m->code, true);
                if (bestmove.code != pvtable[0][0])
                {
                    bestmove.code = pvtable[0][0];
                    if (doPonder) pondermove.code = pvtable[0][1];
                }
                else if (doPonder && pvtable[0][1]) {
                    // use new ponder move
                    pondermove.code = pvtable[0][1];
                }
                alpha = score;
                bestmovescore[0] = score;
                eval_type = HASHEXACT;
            }
            if (score >= beta)
            {
                // Killermove
                if (!ISTACTICAL(m->code))
                {
                    updateHistory(m->code, ms.cmptr, depth * depth);
                    for (int i = 0; i < quietsPlayed - 1; i++)
                    {
                        uint32_t qm = quietMoves[i];
                        updateHistory(qm, ms.cmptr, -(depth * depth));
                    }

                    if (killer[0][0] != m->code)
                    {
                        killer[0][1] = killer[0][0];
                        killer[0][0] = m->code;
                    }
                }
                SDEBUGPRINT(isDebugPv, debugInsert, " Beta-cutoff by move %s: %d", m->toString().c_str(), score);
                tp.addHash(hash, beta, staticeval, HASHBETA, effectiveDepth, (uint16_t)m->code);
                return beta;   // fail hard beta-cutoff
            }
        }
        else if (!isMultiPV)
        {
            // at fail low don't overwrite an existing move
            if (!bestmove.code)
                bestmove = *m;
        }
    }

    SDEBUGPRINT(true, 0, getPv(pvtable[0]).c_str());

    if (isMultiPV)
    {
        if (eval_type == HASHEXACT)
            return bestmovescore[maxmoveindex - 1];
        else
            return alpha;
    }
    else {
        tp.addHash(hash, alpha, staticeval, eval_type, depth, (uint16_t)bestmove.code);
        return alpha;
    }
}


static void uciScore(searchthread *thr, int inWindow, U64 nowtime, int mpvIndex)
{
    int msRun = (int)((nowtime - en.starttime) * 1000 / en.frequency);
    if (inWindow != 1 && (msRun - en.lastReport) < 200)
        return;

    const char* boundscore[] = { "upperbound", "", "lowerbound" };
    char s[4096];
    chessposition *pos = &thr->pos;
    en.lastReport = msRun;
    string pvstring = pos->getPv(mpvIndex ? pos->multipvtable[mpvIndex] : pos->lastpv);
    int score = pos->bestmovescore[mpvIndex];
    U64 nodes = en.getTotalNodes();

    if (!MATEDETECTED(score))
    {
        sprintf_s(s, "info depth %d seldepth %d multipv %d time %d score cp %d %s nodes %llu nps %llu tbhits %llu hashfull %d pv %s\n",
            thr->depth, pos->seldepth, mpvIndex + 1, msRun, score, boundscore[inWindow], nodes,
            (nowtime > en.starttime ? nodes * en.frequency / (nowtime - en.starttime) : 1),
            en.tbhits, tp.getUsedinPermill(), pvstring.c_str());
    }
    else
    {
        int matein = (score > 0 ? (SCOREWHITEWINS - score + 1) / 2 : (SCOREBLACKWINS - score) / 2);
        sprintf_s(s, "info depth %d seldepth %d multipv %d time %d score mate %d nodes %llu nps %llu tbhits %llu hashfull %d pv %s\n",
            thr->depth, pos->seldepth, mpvIndex + 1, msRun, matein, nodes,
            (nowtime > en.starttime ? nodes * en.frequency / (nowtime - en.starttime) : 1),
            en.tbhits, tp.getUsedinPermill(), pvstring.c_str());
    }
    cout << s;
}


template <RootsearchType RT>
static void search_gen1(searchthread *thr)
{
    int score;
    int alpha, beta;
    int deltaalpha = 8;
    int deltabeta = 8;
    int maxdepth;
    int inWindow;
    bool reportedThisDepth;

#ifdef TDEBUG
    en.bStopCount = false;
#endif

    const bool isMultiPV = (RT == MultiPVSearch);
    const bool doPonder = (RT == PonderSearch);

    chessposition *pos = &thr->pos;

    if (en.mate > 0)  // FIXME: Not tested for a long time.
    {
        thr->depth = maxdepth = en.mate * 2;
    }
    else
    {
        thr->lastCompleteDepth = 0;
        thr->depth = 1;
        if (en.maxdepth > 0)
            maxdepth = en.maxdepth;
        else
            maxdepth = MAXDEPTH;
    }

    alpha = SHRT_MIN + 1;
    beta = SHRT_MAX;

    uint32_t lastBestMove = 0;
    int constantRootMoves = 0;
    bool bExitIteration;
    en.lastReport = 0;
    U64 nowtime;
    pos->lastpv[0] = 0;
    do
    {
        inWindow = 1;
        pos->seldepth = thr->depth;
        if (pos->rootmovelist.length == 0)
        {
            // mate / stalemate
            pos->bestmove.code = 0;
            score = pos->bestmovescore[0] =  (pos->isCheckbb ? SCOREBLACKWINS : SCOREDRAW);
            en.stopLevel = ENGINESTOPPED;
        }
        else if (pos->testRepetiton() >= 2 || pos->halfmovescounter >= 100)
        {
            // remis via repetition or 50 moves rule
            pos->bestmove.code = 0;
            if (doPonder) pos->pondermove.code = 0;
            score = pos->bestmovescore[0] = SCOREDRAW;
            en.stopLevel = ENGINESTOPPED;
        }
        else
        {
            score = pos->rootsearch<RT>(alpha, beta, thr->depth);
#ifdef TDEBUG
            if (en.stopLevel == ENGINESTOPIMMEDIATELY && thr->index == 0)
            {
                en.t2stop++;
                en.bStopCount = true;
            }
#endif

            // new aspiration window
            if (score == alpha)
            {
                // research with lower alpha and reduced beta
                beta = (alpha + beta) / 2;
                alpha = max(SHRT_MIN + 1, alpha - deltaalpha);
                deltaalpha += deltaalpha / 4 + 2;
                if (abs(alpha) > 1000)
                    deltaalpha = SHRT_MAX << 1;
                inWindow = 0;
                reportedThisDepth = false;
            }
            else if (score == beta)
            {
                // research with higher beta
                beta = min(SHRT_MAX, beta + deltabeta);
                deltabeta += deltabeta / 4 + 2;
                if (abs(beta) > 1000)
                    deltabeta = SHRT_MAX << 1;
                inWindow = 2;
                reportedThisDepth = false;
            }
            else
            {
                thr->lastCompleteDepth = thr->depth;
                if (score >= en.terminationscore)
                {
                    // bench mode reached needed score
                    en.stopLevel = ENGINEWANTSTOP;
                }
                else if (thr->depth > 4) {
                    // next depth with new aspiration window
                    deltaalpha = 8;
                    deltabeta = 8;
                    if (isMultiPV)
                        alpha = pos->bestmovescore[en.MultiPV - 1] - deltaalpha;
                    else
                        alpha = score - deltaalpha;
                    beta = score + deltabeta;
                }
            }
        }

        // copy new pv to lastpv; preserve identical and longer lastpv
        int i = 0;
        int bDiffers = false;
        while (pos->pvtable[0][i])
        {
            bDiffers = bDiffers || (pos->lastpv[i] != pos->pvtable[0][i]);
            pos->lastpv[i] = pos->pvtable[0][i];
            i++;
        }
        if (bDiffers)
            pos->lastpv[i] = 0;

        if (score > NOSCORE && thr->index == 0)
        {
            nowtime = getTime();

            // search was successfull
            if (isMultiPV)
            {
                if (inWindow == 1)
                {
                    // MultiPV output only if in aspiration window
                    int i = 0;
                    int maxmoveindex = min(en.MultiPV, pos->rootmovelist.length);
                    do
                    {
                        uciScore(thr, inWindow, nowtime, i);
                        i++;
                    } while (i < maxmoveindex);
                }
            }
            else {
                // The only two cases that bestmove is not set can happen if alphabeta hit the TP table or we are in TB
                // so get bestmovecode from there or it was a TB hit so just get the first rootmove
                if (!pos->bestmove.code)
                {
                    uint16_t mc = 0;
                    int dummystaticeval;
                    tp.probeHash(pos->hash, &score, &dummystaticeval, &mc, MAXDEPTH, alpha, beta, 0);
                    pos->bestmove.code = pos->shortMove2FullMove(mc);
                    if (doPonder) pos->pondermove.code = 0;
                }
                    
                // still no bestmove...
                if (!pos->bestmove.code && pos->rootmovelist.length > 0)
                    pos->bestmove.code = pos->rootmovelist.move[0].code;

                if (pos->rootmovelist.length == 1 && !pos->tbPosition && en.endtime1 && !en.isPondering() && pos->lastbestmovescore != NOSCORE)
                    // Don't report score of instamove; use the score of last position instead
                    pos->bestmovescore[0] = pos->lastbestmovescore;

                if (pos->useRootmoveScore)
                    // We have a tablebase score so report this
                    pos->bestmovescore[0] = pos->rootmovelist.move[0].value;

                uciScore(thr, inWindow, nowtime, 0);
            }
        }
        if (inWindow == 1)
        {
            // Skip some depths depending on current depth and thread number using Laser's method
            int cycle = thr->index % 16;
            if (thr->index && (thr->depth + cycle) % SkipDepths[cycle] == 0)
                thr->depth += SkipSize[cycle];

            thr->depth++;
            if (doPonder && en.isPondering() && thr->depth > maxdepth) thr->depth--;  // stay on maxdepth when pondering
            reportedThisDepth = true;
            constantRootMoves++;
        }

        if (lastBestMove != pos->bestmove.code)
        {
            // New best move is found; reset thinking time
            lastBestMove = pos->bestmove.code;
            constantRootMoves = 0;
        }

        // Reset remaining time if depth is finished or new best move is found
        if (thr->index == 0)
        {
            if (inWindow == 1 || !constantRootMoves)
                resetEndTime(constantRootMoves);
            if (!constantRootMoves && en.stopLevel == ENGINESTOPSOON)
                en.stopLevel = ENGINERUN;
        }

        // early exit in playing mode as there is exactly one possible move
        bExitIteration = (pos->rootmovelist.length == 1 && en.endtime1 && !en.isPondering());

        // early exit in TB win/lose position
        bExitIteration = bExitIteration || (pos->tbPosition && abs(score) >= SCORETBWIN - 100 && !en.isPondering());

        // exit if STOPSOON is requested and we're in aspiration window
        bExitIteration = bExitIteration || (en.stopLevel == ENGINESTOPSOON && inWindow == 1);

        // exit if STOPIMMEDIATELY
        bExitIteration = bExitIteration || (en.stopLevel == ENGINESTOPIMMEDIATELY);

        // exit if max depth is reached
        bExitIteration = bExitIteration || (thr->depth > maxdepth);

    } while (!bExitIteration);
    
    if (thr->index == 0)
    {
#ifdef TDEBUG
        if (!en.bStopCount)
            en.t1stop++;
        printf("info string stop info full iteration / immediate:  %4d /%4d\n", en.t1stop, en.t2stop);
#endif
        // Output of best move
        searchthread *bestthr = thr;
        int bestscore = bestthr->pos.bestmovescore[0];
        for (int i = 1; i < en.Threads; i++)
        {
            // search for a better score in the other threads
            searchthread *hthr = &en.sthread[i];
            if (hthr->lastCompleteDepth >= bestthr->lastCompleteDepth
                && hthr->pos.bestmovescore[0] > bestscore)
            {
                bestscore = hthr->pos.bestmovescore[0];
                bestthr = hthr;
            }
        }
        if (pos->bestmove.code != bestthr->pos.bestmove.code)
        {
            // copy best moves and score from best thread to thread 0
            int i = 0;
            while (bestthr->pos.lastpv[i])
            {
                pos->lastpv[i] = bestthr->pos.lastpv[i];
                i++;
            }
            pos->lastpv[i] = 0;
            pos->bestmove = bestthr->pos.bestmove;
            if (doPonder) pos->pondermove = bestthr->pos.pondermove;
            pos->bestmovescore[0] = bestthr->pos.bestmovescore[0];
            inWindow = 1;
            //printf("info string different bestmove from helper  lastpv:%x\n", bestthr->pos.lastpv[0]);
        }

        // remember score for next search in case of an instamove
        en.rootposition.lastbestmovescore = pos->bestmovescore[0];

        if (!reportedThisDepth || bestthr->index)
            uciScore(thr, inWindow, getTime(), 0);

        string strBestmove;
        string strPonder = "";

        if (!pos->bestmove.code)
        {
            // Not enough time to get any bestmove? Fall back to default move
            pos->bestmove = pos->defaultmove;
            if (doPonder) pos->pondermove.code = 0;
        }

        strBestmove = pos->bestmove.toString();

        if (doPonder)
        {
            if (!pos->pondermove.code)
            {
                // Get the ponder move from TT
                pos->playMove(&pos->bestmove);
                uint16_t pondershort = tp.getMoveCode(pos->hash);
                pos->pondermove.code = pos->shortMove2FullMove(pondershort);
                pos->unplayMove(&pos->bestmove);
            }
            if (pos->pondermove.code)
                strPonder = " ponder " + pos->pondermove.toString();
        }

        cout << "bestmove " + strBestmove + strPonder + "\n";

        en.stopLevel = ENGINESTOPPED;
        en.benchmove = strBestmove;
    }

    // Remember depth for benchmark output
    en.benchdepth = thr->depth - 1;
}


void resetEndTime(int constantRootMoves, bool complete)
{
    int timetouse = (en.isWhite ? en.wtime : en.btime);
    int timeinc = (en.isWhite ? en.winc : en.binc);
    int overhead = en.moveOverhead + 8 * en.Threads;

    if (en.movestogo)
    {
        // should garantee timetouse > 0
        // stop soon at 0.9...1.9 x average movetime
        // stop immediately at 1.5...2.5 x average movetime
        int f1 = max(9, 19 - constantRootMoves * 2);
        int f2 = max(15, 25 - constantRootMoves * 2);
        if (complete)
            en.endtime1 = en.starttime + timetouse * en.frequency * f1 / (en.movestogo + 1) / 10000;
        en.endtime2 = en.starttime + min(max(0, timetouse - overhead * en.movestogo), f2 * timetouse / (en.movestogo + 1) / 10) * en.frequency / 1000;
        //printf("info string difftime1=%lld  difftime2=%lld\n", (endtime1 - en.starttime) * 1000 / en.frequency , (endtime2 - en.starttime) * 1000 / en.frequency);
    }
    else if (timetouse) {
        int ph = en.sthread[0].pos.phase();
        if (timeinc)
        {
            // sudden death with increment; split the remaining time in (256-phase) timeslots
            // stop soon after 5..15 timeslot
            // stop immediately after 15..25 timeslots
            int f1 = max(5, 15 - constantRootMoves * 2);
            int f2 = max(15, 25 - constantRootMoves * 2);
            if (complete)
                en.endtime1 = en.starttime + max(timeinc, f1 * (timetouse + timeinc) / (256 - ph)) * en.frequency / 1000;
            en.endtime2 = en.starttime + min(max(0, timetouse - overhead), max(timeinc, f2 * (timetouse + timeinc) / (256 - ph))) * en.frequency / 1000;
        }
        else {
            // sudden death without increment; play for another x;y moves
            // stop soon at 1/32...1/42 time slot
            // stop immediately at 1/12...1/22 time slot
            int f1 = min(42, 32 + constantRootMoves * 2);
            int f2 = min(22, 12 + constantRootMoves * 2);
            if (complete)
                en.endtime1 = en.starttime + timetouse / f1 * en.frequency / 1000;
            en.endtime2 = en.starttime + min(max(0, timetouse - overhead), timetouse / f2) * en.frequency / 1000;
        }
    }
    else if (timeinc)
    {
        // timetouse = 0 => movetime mode: Use exactly timeinc without overhead or early stop
        en.endtime1 = en.endtime2 = en.starttime + timeinc * en.frequency / 1000;
    }
    else {
        en.endtime1 = en.endtime2 = 0;
    }

#ifdef TDEBUG
    printf("info string Time for this move: %4.2f  /  %4.2f\n", (en.endtime1 - en.starttime) / (double)en.frequency, (en.endtime2 - en.starttime) / (double)en.frequency);
#endif
}


void startSearchTime(bool complete = true)
{
    en.starttime = getTime();
    resetEndTime(0, complete);
}


void searchguide()
{
    startSearchTime();

    en.moveoutput = false;
    en.tbhits = en.sthread[0].pos.tbPosition;  // Rootpos in TB => report at least one tbhit

    // increment generation counter for tt aging
    tp.nextSearch();

    if (en.MultiPV == 1 && !en.ponder)
        for (int tnum = 0; tnum < en.Threads; tnum++)
            en.sthread[tnum].thr = thread(&search_gen1<SinglePVSearch>, &en.sthread[tnum]);
    else if (en.ponder)
        for (int tnum = 0; tnum < en.Threads; tnum++)
            en.sthread[tnum].thr = thread(&search_gen1<PonderSearch>, &en.sthread[tnum]);
    else
        for (int tnum = 0; tnum < en.Threads; tnum++)
            en.sthread[tnum].thr = thread(&search_gen1<MultiPVSearch>, &en.sthread[tnum]);

    U64 nowtime;
    while (en.stopLevel != ENGINESTOPPED)
    {
        nowtime = getTime();

        if (nowtime - en.starttime > 3 * en.frequency)
            en.moveoutput = true;

        if (en.stopLevel < ENGINESTOPPED)
        {
            if (en.isPondering())
            {
                Sleep(10);
            }
            else if (en.testPonderHit())
            {
                startSearchTime(false);
                en.resetPonder();
            }
            else if (en.endtime2 && nowtime >= en.endtime2 && en.stopLevel < ENGINESTOPIMMEDIATELY)
            {
                en.stopLevel = ENGINESTOPIMMEDIATELY;
            }
            else if (en.maxnodes && en.maxnodes <= en.getTotalNodes() && en.stopLevel < ENGINESTOPIMMEDIATELY)
            {
                en.stopLevel = ENGINESTOPIMMEDIATELY;
            }
            else if (en.endtime1 && nowtime >= en.endtime1 && en.stopLevel < ENGINESTOPSOON)
            {
                en.stopLevel = ENGINESTOPSOON;
                Sleep(10);
            }
            else {
                Sleep(10);
            }
        }
    }

    // Make the other threads stop now
    en.stopLevel = ENGINESTOPIMMEDIATELY;
    for (int tnum = 0; tnum < en.Threads; tnum++)
        en.sthread[tnum].thr.join();
    en.stopLevel = ENGINETERMINATEDSEARCH;

#ifdef STATISTICS
    search_statistics();
#endif
}

#ifdef STATISTICS
void search_statistics()
{
    U64 n, i1, i2, i3;
    double f0, f1, f2, f3, f4, f5, f6, f10, f11;

    printf("(ST)====Statistics====================================================================================================================================\n");

    // quiescense search statistics
    i1 = statistics.qs_n[0];
    i2 = statistics.qs_n[1];
    n = i1 + i2;
    f0 = 100.0 * i2 / (double)n;
    f1 = 100.0 * statistics.qs_tt / (double)n;
    f2 = 100.0 * statistics.qs_pat / (double)n;
    f3 = 100.0 * statistics.qs_delta / (double)n;
    i3 = statistics.qs_move_delta + statistics.qs_moves;
    f4 =  i3 / (double)statistics.qs_loop_n;
    f5 = 100.0 * statistics.qs_move_delta / (double)i3;
    f6 = 100.0 * statistics.qs_moves_fh / (double)statistics.qs_moves;
    printf("(ST) QSearch: %12lld   %%InCheck:  %5.2f   %%TT-Hits:  %5.2f   %%Std.Pat: %5.2f   %%DeltaPr: %5.2f   Mvs/Lp: %5.2f   %%DlPrM: %5.2f   %%FailHi: %5.2f\n", n, f0, f1, f2, f3, f4, f5, f6);

    // general aplhabeta statistics
    n = statistics.ab_n;
    f0 = 100.0 * statistics.ab_pv / (double)n;
    f1 = 100.0 * statistics.ab_tt / (double)n;
    f2 = 100.0 * statistics.ab_tb / (double)n;
    f3 = 100.0 * statistics.ab_qs / (double)n;
    f4 = 100.0 * statistics.ab_draw_or_win / (double)n;
    printf("(ST) Total AB:%12lld   %%PV-Nodes: %5.2f   %%TT-Hits:  %5.2f   %%TB-Hits: %5.2f   %%QSCalls: %5.2f   %%Draw/Mates: %5.2f\n", n, f0, f1, f2, f3, f4);

    // node pruning
    f0 = 100.0 * statistics.prune_futility / (double)n;
    f1 = 100.0 * statistics.prune_nm / (double)n;
    f2 = 100.0 * statistics.prune_probcut / (double)n;
    f3 = 100.0 * statistics.prune_multicut / (double)n;
    f4 = 100.0 * (statistics.prune_futility + statistics.prune_nm + statistics.prune_probcut + statistics.prune_multicut) / (double)n;
    printf("(ST) Node pruning            %%Futility: %5.2f   %%NullMove: %5.2f   %%ProbeC.: %5.2f   %%MultiC.: %7.5f Total:  %5.2f\n", f0, f1, f2, f3, f4);

    // move statistics
    i1 = statistics.moves_n[0]; // quiet moves
    i2 = statistics.moves_n[1]; // tactical moves
    n = i1 + i2;
    f0 = 100.0 * i1 / (double)n;
    f1 = 100.0 * i2 / (double)n;
    f2 = 100.0 * statistics.moves_pruned_lmp / (double)n;
    f3 = 100.0 * statistics.moves_pruned_futility / (double)n;
    f4 = 100.0 * statistics.moves_pruned_badsee / (double)n;
    f5 = n / (double)statistics.moves_loop_n;
    i3 = statistics.moves_played[0] + statistics.moves_played[1];
    f6 = 100.0 * statistics.moves_fail_high / (double)i3;
    printf("(ST) Moves:   %12lld   %%Quiet-M.: %5.2f   %%Tact.-M.: %5.2f   %%LMP-M.:  %5.2f   %%FutilM.: %5.2f   %%BadSEE: %5.2f  Mvs/Lp: %5.2f   %%FailHi: %5.2f\n", n, f0, f1, f2, f3, f4, f5, f6);

    // late move reduction statistics
    U64 red_n = statistics.red_pi[0] + statistics.red_pi[1];
    f10 = statistics.red_lmr[0] / (double)statistics.red_pi[0];
    f11 = statistics.red_lmr[1] / (double)statistics.red_pi[1];
    f1 = (statistics.red_lmr[0] + statistics.red_lmr[1]) / (double)red_n;
    f2 = statistics.red_history / (double)red_n;
    f3 = statistics.red_pv / (double)red_n;
    f4 = statistics.red_correction / (double)red_n;
    f5 = statistics.red_total / (double)red_n;
    printf("(ST) Reduct.  %12lld   lmr[0]: %4.2f   lmr[1]: %4.2f   lmr: %4.2f   hist: %4.2f   pv: %4.2f   corr: %4.2f   total: %4.2f\n", red_n, f10, f11, f1, f2, f3, f4, f5);

    f0 = 100.0 * statistics.extend_singular / (double)n;
    printf("(ST) Extensions: %%singular: %7.4f\n", f0);

    printf("(ST)==================================================================================================================================================\n");
}
#endif