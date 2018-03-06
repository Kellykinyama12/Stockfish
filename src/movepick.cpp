/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2008 Tord Romstad (Glaurung author)
  Copyright (C) 2008-2015 Marco Costalba, Joona Kiiski, Tord Romstad
  Copyright (C) 2015-2018 Marco Costalba, Joona Kiiski, Gary Linscott, Tord Romstad

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <cassert>

#include "movepick.h"

namespace {

  enum Stages {
    MAIN_TT, CAPTURE_INIT, GOOD_CAPTURE, KILLER0, KILLER1, COUNTERMOVE, QUIET_INIT, QUIET, BAD_CAPTURE,
    EVASION_TT, EVASION_INIT, EVASION,
    PROBCUT_TT, PROBCUT_INIT, PROBCUT,
    QSEARCH_TT, QCAPTURE_INIT, QCAPTURE, QCHECK_INIT, QCHECK
  };

  // partial_insertion_sort() sorts moves in descending order up to and including
  // a given limit. The order of moves smaller than the limit is left unspecified.
  void partial_insertion_sort(ExtMove* begin, ExtMove* end, int limit) {

    for (ExtMove *sortedEnd = begin, *p = begin + 1; p < end; ++p)
        if (p->value >= limit)
        {
            ExtMove tmp = *p, *q;
            *p = *++sortedEnd;
            for (q = sortedEnd; q != begin && *(q - 1) < tmp; --q)
                *q = *(q - 1);
            *q = tmp;
        }
  }

} // namespace


/// Constructors of the MovePicker class. As arguments we pass information
/// to help it to return the (presumably) good moves first, to decide which
/// moves to return (in the quiescence search, for instance, we only want to
/// search captures, promotions, and some checks) and how important good move
/// ordering is at the current node.

/// MovePicker constructor for the main search
MovePicker::MovePicker(const Position& p, Move ttm, Depth d, const ButterflyHistory* mh,
                       const CapturePieceToHistory* cph, const PieceToHistory** ch, Move cm, Move* killers_p)
           : pos(p), mainHistory(mh), captureHistory(cph), contHistory(ch), countermove(cm),
             killers{killers_p[0], killers_p[1]}, depth(d){

  assert(d > DEPTH_ZERO);

  stage = pos.checkers() ? EVASION_TT : MAIN_TT;
  ttMove = ttm && pos.pseudo_legal(ttm) ? ttm : MOVE_NONE;
  stage += (ttMove == MOVE_NONE);
}

/// MovePicker constructor for quiescence search
MovePicker::MovePicker(const Position& p, Move ttm, Depth d, const ButterflyHistory* mh,
                       const CapturePieceToHistory* cph, Square rs)
           : pos(p), mainHistory(mh), captureHistory(cph), recaptureSquare(rs), depth(d) {

  assert(d <= DEPTH_ZERO);

  stage = pos.checkers() ? EVASION_TT : QSEARCH_TT;
  ttMove =    ttm
           && pos.pseudo_legal(ttm)
           && (depth > DEPTH_QS_RECAPTURES || to_sq(ttm) == recaptureSquare) ? ttm : MOVE_NONE;
  stage += (ttMove == MOVE_NONE);
}

/// MovePicker constructor for ProbCut: we generate captures with SEE higher
/// than or equal to the given threshold.
MovePicker::MovePicker(const Position& p, Move ttm, Value th, const CapturePieceToHistory* cph)
           : pos(p), captureHistory(cph), threshold(th) {

  assert(!pos.checkers());

  stage = PROBCUT_TT;
  ttMove =   ttm
          && pos.pseudo_legal(ttm)
          && pos.capture(ttm)
          && pos.see_ge(ttm, threshold) ? ttm : MOVE_NONE;
  stage += (ttMove == MOVE_NONE);
}

/// score() assigns a numerical value to each move in a list, used for sorting.
/// Captures are ordered by Most Valuable Victim (MVV), preferring captures
/// with a good history. Quiets are ordered using the histories.
template<GenType Type>
void MovePicker::score() {

  static_assert(Type == CAPTURES || Type == QUIETS || Type == EVASIONS, "Wrong type");

  for (auto& m : *this)
      if (Type == CAPTURES)
          m.value =  PieceValue[MG][pos.piece_on(to_sq(m))]
                   + (*captureHistory)[pos.moved_piece(m)][to_sq(m)][type_of(pos.piece_on(to_sq(m)))];

      else if (Type == QUIETS)
          m.value =  (*mainHistory)[pos.side_to_move()][from_to(m)]
                   + (*contHistory[0])[pos.moved_piece(m)][to_sq(m)]
                   + (*contHistory[1])[pos.moved_piece(m)][to_sq(m)]
                   + (*contHistory[3])[pos.moved_piece(m)][to_sq(m)];

      else // Type == EVASIONS
      {
          if (pos.capture(m))
              m.value =  PieceValue[MG][pos.piece_on(to_sq(m))]
                       - Value(type_of(pos.moved_piece(m)));
          else
              m.value = (*mainHistory)[pos.side_to_move()][from_to(m)] - (1 << 28);
      }
}

/// pick() returns the next (best) move satisfying a predicate function
template<PickType T, typename Pred>
Move MovePicker::select_move(Pred filter) {

  while (cur < endMoves)
  {
      if (T == BEST_SCORE)
          std::swap(*cur, *std::max_element(cur, endMoves));

      move = *cur++;

      if (filter())
          return move;
  }
  return move = MOVE_NONE;
}

/// next_move() is the most important method of the MovePicker class. It returns
/// a new pseudo legal move every time it is called, until there are no more moves
/// left. It picks the move with the highest score from a list of generated moves.
Move MovePicker::next_move(bool skipQuiets) {

again:
  switch (stage) {

  case MAIN_TT:
  case EVASION_TT:
  case QSEARCH_TT:
  case PROBCUT_TT:
  {
      ++stage;
      return ttMove;
  }

  case CAPTURE_INIT:
  case PROBCUT_INIT:
  case QCAPTURE_INIT:
  {
      endBadCaptures = cur = moves;
      endMoves = generate<CAPTURES>(pos, cur);
      score<CAPTURES>();
      if (++stage != GOOD_CAPTURE)
          goto again;
  }

  case GOOD_CAPTURE:
  {
      auto filter = [&](){ return   move != ttMove
                                 && (pos.see_ge(move, Value(-55 * (cur-1)->value / 1024))
                                     ? true 
                                     // Move losing capture to endBadCaptures to be tried later
                                     : (*endBadCaptures++ = move, false)); };
      if (select_move<BEST_SCORE>(filter))
          return move;
      ++stage;
  }

  case KILLER0:
  case KILLER1:
  {
      do
      {
          move = killers[++stage - KILLER1];
          auto filter =    move != MOVE_NONE
                       &&  move != ttMove
                       &&  pos.pseudo_legal(move)
                       && !pos.capture(move);
          if (filter)
              return move;
      } while (stage <= KILLER1);
  }

  case COUNTERMOVE:
  {
      ++stage;
      move = countermove;
      auto filter =    move != MOVE_NONE
                   &&  move != ttMove
                   &&  move != killers[0]
                   &&  move != killers[1]
                   &&  pos.pseudo_legal(move)
                   && !pos.capture(move);
      if (filter)
          return move;
  }

  case QUIET_INIT:
  {
      cur = endBadCaptures;
      endMoves = generate<QUIETS>(pos, cur);
      score<QUIETS>();
      partial_insertion_sort(cur, endMoves, -4000 * depth / ONE_PLY);
      ++stage;
  }

  case QUIET:
  {
      if (!skipQuiets)
      {
          auto filter = [&](){ return   move != ttMove
                                     && move != killers[0]
                                     && move != killers[1]
                                     && move != countermove; };
          if (select_move<NEXT>(filter))
              return move;
      }
      cur = moves, endMoves = endBadCaptures; // Point to beginning and end of bad captures
      ++stage;
  }

  case BAD_CAPTURE:
  {
      auto filter = [&](){ return move != ttMove; };
      return select_move<NEXT>(filter);
  }

  case EVASION_INIT:
  {
      cur = moves;
      endMoves = generate<EVASIONS>(pos, cur);
      score<EVASIONS>();
      ++stage;
  }

  case EVASION:
  {
      auto filter = [&](){ return move != ttMove; };
      return select_move<BEST_SCORE>(filter);
  }

  case PROBCUT:
  {
      auto filter = [&](){ return   move != ttMove
                                 && pos.see_ge(move, threshold); };
      return select_move<BEST_SCORE>(filter);
  }

  case QCAPTURE:
  {
      auto filter = [&](){ return   move != ttMove
                                 && (depth > DEPTH_QS_RECAPTURES || to_sq(move) == recaptureSquare); };
      if (select_move<BEST_SCORE>(filter))
          return move;

      // If we don't have to try checks then we have finished
      if (depth != DEPTH_QS_CHECKS)
          return MOVE_NONE;

      ++stage;
  }

  case QCHECK_INIT:
  {
      cur = moves;
      endMoves = generate<QUIET_CHECKS>(pos, cur);
      ++stage;
  }

  case QCHECK:
  {
      auto filter = [&](){ return move != ttMove; };
      return select_move<NEXT>(filter);
  }

  default:
      assert(false);
      return MOVE_NONE;
  }
}
