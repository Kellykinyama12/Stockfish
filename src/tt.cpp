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

#include <cstring>   // For std::memset
#include <iostream>
#include <fstream> //from kellykynyama mcts

#include "bitboard.h"
#include "tt.h"

//from Kelly Begin
using namespace std;
LearningHashTable globalLearningHT,experienceHT;
//from Kelly end

TranspositionTable TT; // Our global transposition table


/// TranspositionTable::resize() sets the size of the transposition table,
/// measured in megabytes. Transposition table consists of a power of 2 number
/// of clusters and each cluster consists of ClusterSize number of TTEntry.

void TranspositionTable::resize(size_t mbSize) {

  size_t newClusterCount = size_t(1) << msb((mbSize * 1024 * 1024) / sizeof(Cluster));

  if (newClusterCount == clusterCount)
      return;

  clusterCount = newClusterCount;

  free(mem);
  mem = calloc(clusterCount * sizeof(Cluster) + CacheLineSize - 1, 1);

  if (!mem)
  {
      std::cerr << "Failed to allocate " << mbSize
                << "MB for transposition table." << std::endl;
      exit(EXIT_FAILURE);
  }

  table = (Cluster*)((uintptr_t(mem) + CacheLineSize - 1) & ~(CacheLineSize - 1));
}


/// TranspositionTable::clear() overwrites the entire transposition table
/// with zeros. It is called whenever the table is resized, or when the
/// user asks the program to clear the table (from the UCI interface).

void TranspositionTable::clear() {

  std::memset(table, 0, clusterCount * sizeof(Cluster));
}


/// TranspositionTable::probe() looks up the current position in the transposition
/// table. It returns true and a pointer to the TTEntry if the position is found.
/// Otherwise, it returns false and a pointer to an empty or least valuable TTEntry
/// to be replaced later. The replace value of an entry is calculated as its depth
/// minus 8 times its relative age. TTEntry t1 is considered more valuable than
/// TTEntry t2 if its replace value is greater than that of t2.

TTEntry* TranspositionTable::probe(const Key key, bool& found) const {

  TTEntry* const tte = first_entry(key);
  const uint16_t key16 = key >> 48;  // Use the high 16 bits as key inside the cluster

  for (int i = 0; i < ClusterSize; ++i)
      if (!tte[i].key16 || tte[i].key16 == key16)
      {
          if ((tte[i].genBound8 & 0xFC) != generation8 && tte[i].key16)
              tte[i].genBound8 = uint8_t(generation8 | tte[i].bound()); // Refresh

          return found = (bool)tte[i].key16, &tte[i];
      }

  // Find an entry to be replaced according to the replacement strategy
  TTEntry* replace = tte;
  for (int i = 1; i < ClusterSize; ++i)
      // Due to our packed storage format for generation and its cyclic
      // nature we add 259 (256 is the modulus plus 3 to keep the lowest
      // two bound bits from affecting the result) to calculate the entry
      // age correctly even after generation8 overflows into the next cycle.
      if (  replace->depth8 - ((259 + generation8 - replace->genBound8) & 0xFC) * 2
          >   tte[i].depth8 - ((259 + generation8 -   tte[i].genBound8) & 0xFC) * 2)
          replace = &tte[i];

  return found = false, replace;
}


/// TranspositionTable::hashfull() returns an approximation of the hashtable
/// occupation during a search. The hash is x permill full, as per UCI protocol.

int TranspositionTable::hashfull() const {

  int cnt = 0;
  for (int i = 0; i < 1000 / ClusterSize; i++)
  {
      const TTEntry* tte = &table[i].entry[0];
      for (int j = 0; j < ClusterSize; j++)
          if ((tte[j].genBound8 & 0xFC) == generation8)
              cnt++;
  }
  return cnt;
}

//from Kelly begin
void loadLearningFileIntoLearningTables(bool toDeleteBinFile) {
  std::string fileName="experience";
  ifstream inputLearningFile("experience.bin", ios::in | ios::binary);
  int loading = 1;
  while (loading)
  {
    LearningFileEntry currentInputLearningFileEntry;
    currentInputLearningFileEntry.depth = DEPTH_ZERO;
    currentInputLearningFileEntry.hashKey = 0;
    currentInputLearningFileEntry.move = MOVE_NONE;
    currentInputLearningFileEntry.score = VALUE_NONE;
    inputLearningFile.read((char*)&currentInputLearningFileEntry, sizeof(currentInputLearningFileEntry));
    if (currentInputLearningFileEntry.hashKey)
    {
      insertIntoOrUpdateLearningTable(currentInputLearningFileEntry,globalLearningHT);
      if(toDeleteBinFile)
      {
	 insertIntoOrUpdateLearningTable(currentInputLearningFileEntry,experienceHT);
      }
    }
    else
      loading = 0;
  }
  inputLearningFile.close();
  if(toDeleteBinFile)
  {
    char fileNameStr[fileName.size() + 1];
    strcpy(fileNameStr, fileName.c_str());
    remove(fileNameStr);
  }
}

void insertIntoOrUpdateLearningTable(LearningFileEntry& fileExpEntry,LearningHashTable& learningHT)
{
    // We search in the range of all the hash table entries with key fileExpEntry
    auto range = learningHT.equal_range(fileExpEntry.hashKey);
    auto it1 = range.first;
    auto it2 = range.second;

    bool isNewNode = true;
    while (it1 != it2)
    {
      expNode node = &(it1->second);
      if (node->hashKey == fileExpEntry.hashKey)
	{
	  isNewNode = false;
	  bool newSibling = true;
	  for(int k = 0; k < node->siblings; k++)
	  {
		if(node->siblingsMoveInfo[k].move == fileExpEntry.move)
		{
			newSibling = false;
			if(node->siblingsMoveInfo[k].depth <= fileExpEntry.depth)
			{
				//update lateChild begin
				node->siblingsMoveInfo[k].visits++;
				node->siblingsMoveInfo[k].move = fileExpEntry.move;
				node->siblingsMoveInfo[k].score = fileExpEntry.score;
				node->siblingsMoveInfo[k].depth = fileExpEntry.depth;
				//update lateChild end	
				if((node->latestMoveInfo.depth <= node->siblingsMoveInfo[k].depth && node->siblingsMoveInfo[k].depth != DEPTH_ZERO)
					||((node->latestMoveInfo.depth == DEPTH_ZERO && node->latestMoveInfo.depth <= node->siblingsMoveInfo[k].depth)
						&& node->siblingsMoveInfo[k].visits >= node->latestMoveInfo.visits)
					)
				{
					//update lateChild begin
					node->latestMoveInfo = node->siblingsMoveInfo[k];
				}
			}
			break;
		}		  
	  }
	  if(newSibling)
	  {
		 node->siblings++;
		 //update lateChild begin
		node->siblingsMoveInfo[node->siblings].visits=1;
		node->siblingsMoveInfo[node->siblings].move = fileExpEntry.move;
		node->siblingsMoveInfo[node->siblings].score = fileExpEntry.score;
		node->siblingsMoveInfo[node->siblings].depth = fileExpEntry.depth;
		//update lateChild end	
		
		if((node->latestMoveInfo.depth <= node->siblingsMoveInfo[node->siblings].depth && node->siblingsMoveInfo[node->siblings].depth != DEPTH_ZERO)
			||((node->latestMoveInfo.depth == DEPTH_ZERO && node->latestMoveInfo.depth <= node->siblingsMoveInfo[node->siblings].depth)
				&& node->siblingsMoveInfo[node->siblings].visits >= node->latestMoveInfo.visits)
		)
		{
			//update lateChild begin
			node->latestMoveInfo = node->siblingsMoveInfo[node->siblings];
		}
	  }
	  //exit the position
	  break;
	}
      it1++;
    }

    if (isNewNode)
    {
      // Node was not found, so we have to create a new one
      expNodeInfo infos;
      infos.hashKey = fileExpEntry.hashKey;
      infos.latestMoveInfo.move = fileExpEntry.move;
      infos.latestMoveInfo.score = fileExpEntry.score;
      infos.latestMoveInfo.depth = fileExpEntry.depth;
	  infos.latestMoveInfo.visits = 1;
	  infos.siblingsMoveInfo[0] = infos.latestMoveInfo;
	  infos.siblings = 1;
	  
      learningHT.insert(make_pair(fileExpEntry.hashKey, infos));
    }
}

/// getNodeFromGlobalHT(Key key) probes the Monte-Carlo hash table to return the node with the given
/// position or a nullptr Node if it doesn't exist yet in the table.
expNode getNodeFromHT(Key key,HashTableType hashTableType)
{
  // We search in the range of all the hash table entries with key key.
  expNode currentNode = nullptr;
  auto range=globalLearningHT.equal_range(key);
  if(hashTableType==HashTableType::experience)
    {
      range=experienceHT.equal_range(key);
    }
  auto it1 = range.first;
  auto it2 = range.second;
  while (it1 != it2)
  {
    currentNode = &(it1->second);
    if (currentNode->hashKey == key)
    {
	return currentNode;
    }
    it1++;
  }

  return currentNode;
}

void writeLearningFile(HashTableType hashTableType)
{
  std::ofstream outputFile ("experience.bin", std::ofstream::trunc | std::ofstream::binary);
  LearningHashTable currentLearningHT;
  currentLearningHT=experienceHT;
  if(hashTableType==HashTableType::global)
    {
      currentLearningHT=globalLearningHT;
    }
  for(auto& it:currentLearningHT)
  {
    LearningFileEntry currentFileExpEntry;
    expNodeInfo currentNodeInfo=it.second;
	for(int k=0; k <currentNodeInfo.siblings; k++)
	{
		MoveInfo currentLatestMoveInfo=currentNodeInfo.siblingsMoveInfo[k];
		currentFileExpEntry.depth = currentLatestMoveInfo.depth;
		currentFileExpEntry.hashKey = it.first;
		currentFileExpEntry.move = currentLatestMoveInfo.move;
		currentFileExpEntry.score = currentLatestMoveInfo.score;
		outputFile.write((char*)&currentFileExpEntry, sizeof(currentFileExpEntry));
	}
  }
  outputFile.close();
}

void loadSlaveLearningFilesIntoLearningTables()
{
    bool merging=true;
    int i=0;
    while (merging)
    {
      std::string index = std::to_string(i);
      std::string slaveFileName ="";
      slaveFileName="experience" + index + ".bin";
      ifstream slaveInputFile (slaveFileName, ios::in | ios::binary);
      if(!slaveInputFile.good())
      {
	merging=false;
	i++;
      }
      else
      {
	while(slaveInputFile.good())
	{
	  LearningFileEntry slaveFileExpEntry;
	  slaveFileExpEntry.depth = DEPTH_ZERO;
	  slaveFileExpEntry.hashKey = 0;
	  slaveFileExpEntry.move = MOVE_NONE;
	  slaveFileExpEntry.score = VALUE_NONE;

	  slaveInputFile.read((char*)&slaveFileExpEntry, sizeof(slaveFileExpEntry));
	  if (slaveFileExpEntry.hashKey)
	  {
	      insertIntoOrUpdateLearningTable(slaveFileExpEntry,experienceHT);
	  }
	  else
	  {
	    slaveInputFile.close();
	    char slaveStr[slaveFileName.size() + 1];
	    strcpy(slaveStr, slaveFileName.c_str());
	    remove(slaveStr);
	    i++;
	  }
	}
      }
    }
}
//from Kelly End
