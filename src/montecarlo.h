/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2008 Tord Romstad (Glaurung author)
  Copyright (C) 2008-2015 Marco Costalba, Joona Kiiski, Tord Romstad
  Copyright (C) 2015-2017 Marco Costalba, Joona Kiiski, Gary Linscott, Tord Romstad

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

#ifndef MONTECARLO_H_INCLUDED
#define MONTECARLO_H_INCLUDED

#include <cassert>
#include <deque>
#include <memory> // For std::unique_ptr
#include <string>

#include "bitboard.h"
#include "types.h"
#include "tree-3.1/tree.hh"


typedef double Reward;


/// UCTInfo class stores information in a node

class UCTInfo {
public:

  Move last_move() { return lastMove; }

  // Data members
  uint64_t visits        = 0;         // number of visits by the UCT algorithm
  uint64_t sons          = 0;         // total number of legal moves
  uint64_t expandedSsons = 0;         // number of sons expanded by the UCT algorithm
  Reward   reward        = 0.0;       // reward from the point of view of the side to move
  Move     lastMove      = MOVE_NONE; // the move between the parent and this node
};


typedef tree<UCTInfo> Node;


UCTInfo get_uct_infos(Node n) {
  return n.begin().node->data;
}

Move move_of(Node n) {
    return get_uct_infos(n).last_move();
}



/// A list to keep track of the position states along the setup moves (from the
/// start position to the position just before the search starts). Needed by
/// 'draw by repetition' detection. Use a std::deque because pointers to
/// elements are not invalidated upon list resizing.
typedef std::unique_ptr<std::deque<StateInfo>> StateListPtr;



#endif // #ifndef MONTECARLO_H_INCLUDED
