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

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring> // For std::memset, std::memcmp
#include <iostream>
#include <sstream>

#include "misc.h"
#include "montecarlo.h"
#include "position.h"
#include "search.h"
#include "thread.h"
#include "types.h"
#include "uci.h"



// UCT is a class implementing Monte-Carlo Tree Search for Stockfish.
// We are following the survey http://mcts.ai/pubs/mcts-survey-master.pdf
// for the Monte-Carlo algorithm description and the notations used.

// Bibliography:
//     http://mcts.ai/pubs/mcts-survey-master.pdf
//     https://www.ke.tu-darmstadt.de/lehre/arbeiten/bachelor/2012/Arenz_Oleg.pdf
//     https://dke.maastrichtuniversity.nl/m.winands/publications.html
//     https://www.ru.is/faculty/yngvi/pdf/WinandsB11a.pdf
//     https://www.nature.com/articles/nature24270.epdf?author_access_token=VJXbVjaSHxFoctQQ4p2k4tRgN0jAjWel9jnR3ZoTv0PVW4gB86EEpGqTRDtpIz-2rmo8-KG06gqVobU5NSCFeHILHcVFUeMsbvwS-lxjqQGg98faovwjxeTUgZAUMnRQ


using namespace std;
using std::string;


UCTHashTable UCTTable;

Node get_node(const Position& pos) {

   Key key1 = pos.key();
   Key key2 = pos.pawn_key();
   Node node = UCTTable[key1];

   // If node already exists, return it
   if (node->key1 == key1 && node->key2 == key2)
       return node;

   // Otherwise create a new node. This will overwrite any node in the
   // hash table in the same location.

   node->key1         = key1;
   node->key2         = key2;
   node->visits       = 0;         // number of visits by the UCT algorithm
   node->sons         = 0;         // total number of legal moves
   node->expandedSons = 0;         // number of sons expanded by the UCT algorithm
   node->lastMove     = MOVE_NONE; // the move between the parent and this node

   return node;
}

Move move_of(Node node) { return node->last_move(); }
Edge* get_list_of_edges(Node node) { return node->edges_list(); }
int number_of_sons(Node node) { return node->sons; }

// UCT::search() is the main function of UCT algorithm.

Move UCT::search() {

    create_root();

    while (computational_budget()) {
       print_stats();
       Node node = tree_policy();
       Reward reward = playout_policy(node);
       backup(node, reward);
    }

    return best_move(root, 0.0);
}


/// UCT::UCT() is the constructor for the UCT class
UCT::UCT(Position& p) : pos(p) {
    create_root();
}

/// UCT::create_root() initializes the UCT tree with the given position
void UCT::create_root() {

    // Initialize the global counters
    doMoveCnt  = 0;
    descentCnt = 0;
    playoutCnt = 0;
    priorCnt   = 0;

    // Prepare the stack to go down and up in the game tree
    ply = 1;
    std::memset(stackBuffer, 0, sizeof(stackBuffer));
    for (int i = -4; i <= MAX_PLY + 2; i++)
      stack[i].contHistory = &(pos.this_thread()->contHistory[NO_PIECE][0]); // Use as sentinel

    // TODO : what to do with killers ???

    // TODO : setupStates should probably come the caller, as a global ???
    StateListPtr setupStates(new std::deque<StateInfo>(1));

    // Save a hard copy of the root position
    StateInfo tmp = setupStates->back();
    rootPosition.set(pos.fen(), pos.is_chess960(), &setupStates->back(), pos.this_thread());
    setupStates->back() = tmp;

    // Erase the list of nodes, and set the current node to the root node
    std::memset(nodesBuffer, 0, sizeof(nodesBuffer));
    root = nodes[ply] = get_node(pos);

    if (current_node()->visits == 0)
       generate_moves();

    assert(ply == 1);
    assert(root == nodes[ply]);
    assert(root == current_node());
}


/// UCT::computational_budget() stops the search if the computational budget
/// has been reached (time limit, or number of nodes, etc.)
bool UCT::computational_budget() {
    assert(current_node() == root);

    return (descentCnt < 5);
}


/// UCT::tree_policy() selects the next node to be expanded
Node UCT::tree_policy() {

    assert(current_node() == root);
    descentCnt++;

    double C = get_exploration_constant();

    while (current_node()->visits > 0) {

        C = get_exploration_constant();
        Move m = best_move(current_node(), C);

        assert(pos.legal(m));

        do_move(m);
        
        nodes[ply] = get_node(pos); // Set current node
    }

    assert(current_node()->visits == 0);

    return current_node();
}


/// UCT::playout_policy() expand the selected node, plays a semi random game starting 
/// from there, and return the reward of this playout from the point of view of the
/// player to move in the expanded move.
Reward UCT::playout_policy(Node node) {

    
    assert(current_node()->visits == 0);
    playoutCnt++;
    
    Node old = current_node();

    generate_moves();
    print_stats();
    
    assert(current_node()->visits == 1);
    assert(current_node()->sons > 0);
    assert(current_node() == old);
    
    // TODO : what if there is no legal moves? Handle stalemate and mate !!

    return get_list_of_edges(current_node())[0].prior;
}


/// UCT::UCB() calculates the upper confidence bound formula for the son
/// which we reach from node "node" by following the edge "edge".
double UCT::UCB(Node node, Edge& edge, double C) {

    int fatherVisits = node->visits;

    assert(fatherVisits > 0);

    double result = 0.0;

    if (edge.visits)
        result += edge.actionValue / edge.visits;

    result += C * edge.prior * sqrt(fatherVisits) / (1 + edge.visits);

    return result;
}

/// UCT::backup() implements the strategy for accumulating rewards up
/// the tree after a playout.
void UCT::backup(Node node, Reward r) {


   assert(current_node() == root);
}


/// UCT::best_move() selects the best child of a node according to the UCB formula
Move UCT::best_move(Node node, double C) {

    cerr << "Entering best_move()..." << endl;
    cerr << pos << endl;

    Edge* edges = get_list_of_edges(node);
    Move best = MOVE_NONE;

    for (int k = 0 ; k < number_of_sons(node) ; k++)
    {
        cerr << "move #" << k << ": "
            << UCI::move(edges[k].move, pos.is_chess960())
            << " with prior " << edges[k].prior
            << endl;
    }

    double bestValue = -100000000.0;
    for (int k = 0 ; k < number_of_sons(node) ; k++)
    {
        double r = UCB(node, edges[k], C);
        if ( r > bestValue )
        {
            bestValue = r;
            best = edges[k].move;
        }
    }

    cerr << "selecting move " << UCI::move(best, pos.is_chess960())
         << " with UCB " << bestValue
         << endl;
    cerr << "...exiting best_move()" << endl;

    return best;
}

/// UCT::set_exploration_constant() changes the exploration constant of the UCB formula.
///
/// This constant sets the balance between the exploitation of past results and the
/// exploration of new branches in the UCT tree. The higher the constant, the more
/// likely is the algorithm to explore new parts of the tree, whereas lower values
/// of the constant makes an algorithm which focuses more on the already explored
/// parts of the tree. Default value is 10.0
///
void UCT::set_exploration_constant(double C) {
    explorationConstant = C;
}

/// UCT::get_exploration_constant() returns the exploration constant of the UCB formula
double UCT::get_exploration_constant() {
    return explorationConstant;
}

/// UCT::current_node() is the current node of our tree exploration
Node UCT::current_node() {
    return nodes[ply];
}

/// UCT::do_move() plays a move in the search tree from the current position
void UCT::do_move(Move m) {

    doMoveCnt++;

    stack[ply].ply         = ply;
    stack[ply].currentMove = m;
    stack[ply].contHistory = &(pos.this_thread()->contHistory[pos.moved_piece(m)][to_sq(m)]);

    pos.do_move(m, states[ply]);

    ply++;
}


/// UCT::undo_move() undo the current move in the search tree
void UCT::undo_move() {
    ply--;
    pos.undo_move(stack[ply].currentMove);
}

/// UCT::add_prior_to_node() adds the given (move,prior) pair as a new son for a node
void UCT::add_prior_to_node(Node node, Move m, Reward prior, int moveCount) {

   assert(node->sons < MAX_EDGES);

   int n = node->sons;
   if (n < MAX_EDGES)
   {
       node->edges[n].visits          = 0;
       node->edges[n].move            = m;
       node->edges[n].prior           = prior;
       node->edges[n].actionValue     = 0.0;
       node->edges[n].meanActionValue = 0.0;
       node->sons++;

       cerr << "Adding move #" << n << ": "
            << UCI::move(m, pos.is_chess960())
            << " with prior " << prior
            << endl;

       assert(node->sons == moveCount);
   }
   else
   {
        cerr << "ERROR : too many sons (" << node->sons << ") in add_prior_to_node()" << endl;
   }
}


/// UCT::generate_moves() does some Stockfish gimmick to iterate over legal moves
/// of the current position, in a sensible order.
/// For historical reasons, it is not so easy to get a MovePicker object to
/// generate moves if we want to have a decent order (captures first, then
/// quiet moves, etc.). We have to pass various history tables to the MovePicker
/// constructor, like in the alpha-beta implementation of move ordering.
void UCT::generate_moves() {

    assert(current_node()->visits == 0);

    cerr << "Entering generate_moves()..." << endl;
    cerr << pos << endl;

    print_node(current_node());

    Thread*  thread      = pos.this_thread();
    Square   prevSq      = to_sq(stack[ply-1].currentMove);
    Move     countermove = thread->counterMoves[pos.piece_on(prevSq)][prevSq];
    Move     ttMove      = MOVE_NONE;  // FIXME
    Move*    killers     = stack[ply].killers;
    Depth    depth       = 30 * ONE_PLY;

    const CapturePieceToHistory* cph   = &thread->captureHistory;
    const ButterflyHistory* mh         = &thread->mainHistory;
    const PieceToHistory*   contHist[] = { stack[ply-1].contHistory,
                                           stack[ply-2].contHistory,
                                           nullptr,
                                           stack[ply-4].contHistory };

    MovePicker mp(pos, ttMove, depth, mh, cph, contHist, countermove, killers);

    Move move;
    Reward prior;
    int moveCount = 0;

    // Generate the legal moves and calculate their priors
    while ((move = mp.next_move()) != MOVE_NONE)
        if (pos.legal(move))
        {
            stack[ply].moveCount = ++moveCount;

            prior = calculate_prior(move, moveCount);

            add_prior_to_node(current_node(), move, prior, moveCount);

        }

    // Sort the moves according to their prior value
    int n = number_of_sons(current_node());
    if (n > 0)
    {
        Edge* edges = get_list_of_edges(current_node());
        std::sort(edges, edges + n, ComparePrior);
    }

    // Indicate that we have just expanded the current node
    Node s = current_node();
    s->visits       = 1;
    s->expandedSons = 0;
}

/// UCT::evaluate_with_minimax() evaluates the current position in the tree
/// with a small minimax search of the given depth. Note : you can use 
/// depth==DEPTH_ZERO for a direct quiescence value.
Value UCT::evaluate_with_minimax(Depth depth) {

    stack[ply].ply          = ply;
    stack[ply].currentMove  = MOVE_NONE;
    stack[ply].excludedMove = MOVE_NONE;

    return minimax_value(pos, &stack[ply], depth);
}

/// UCT::calculate_prior() returns the a-priori reward of the move leading to
/// the n-th son of the current node. Here we use the evaluation function to
/// estimate this prior, we could use other strategies too (like the rank n of
/// the son, or the type of the move (good capture/quiet/bad capture), etc).
Reward UCT::calculate_prior(Move move, int n) {

    assert(n >= 0);

    priorCnt++;

    do_move(move);
    //Reward prior = value_to_reward(evaluate_with_minimax(3 * ONE_PLY));
    Reward prior = value_to_reward(evaluate_with_minimax(DEPTH_ZERO));
    undo_move();

    return prior;
}

/// UCT::value_to_reward() transforms a Stockfish value to a reward in [0..1]
/// We scale the logistic function such that a value of 600 (about three pawns)
/// is given a probability of win of 0.75, and a value of -600 is given a probability
/// of win of 0.25
Reward UCT::value_to_reward(Value v)
{
    const double k = -0.00183102048111;
    double r = 1.0 / (1 + exp(k * int(v)));
    return Reward(r);
}

/// UCT::reward_to_value() transforms a reward in [0..1] to a Stockfish value.
/// The scale is such that a reward of 0.75 corresponds to 600 (about three pawns),
/// and a reward of 0.25 corresponds to -600 (about minus three pawns).
Value UCT::reward_to_value(Reward r)
{
    if (r > 0.99) return  VALUE_KNOWN_WIN;
    if (r < 0.01) return -VALUE_KNOWN_WIN;

    const double g = 546.14353597715121;  //  this is 1 / k
    double v = g * log(r / (1.0 - r)) ;
    return Value(int(v));
}


/// UCT::test()
void UCT::test() {
   cerr << "---------------------------------------------------------------------------------" << endl;
   cerr << "Testing UCT for position..." << endl;
   cerr << pos << endl;

   search();

   cerr << "...end of UCT testing!" << endl;
   cerr << "---------------------------------------------------------------------------------" << endl;
}


/// UCT::print_stats()
void UCT::print_stats() {
   cerr << "ply        = " << ply        << endl;
   cerr << "descentCnt = " << descentCnt << endl;
   cerr << "playoutCnt = " << playoutCnt << endl;
   cerr << "doMoveCnt  = " << doMoveCnt  << endl;
   cerr << "priorCnt   = " << priorCnt   << endl;
}

/// UCT::print_node()
void UCT::print_node(Node node) {
   cerr << "isCurrent    = " << (node == current_node()) << endl;
   cerr << "isRoot       = " << (node == root)           << endl;
   cerr << "key1         = " << node->key1               << endl;
   cerr << "key2         = " << node->key2               << endl;
   cerr << "visits       = " << node->visits             << endl;
   cerr << "sons         = " << node->sons               << endl;
   cerr << "expandedSons = " << node->expandedSons       << endl;
}





















