/*
 * contig_processor.hpp
 *
 *  Created on: Jun 27, 2014
 *      Author: lab42
 */

#pragma once
#include "sam_reader.hpp"
#include "read.hpp"
#include "include.hpp"



class ContigProcessor {
	string sam_file;
	string contig_file;
//	static const map<char, char> nt_to_pos;
//	static const map< char, char> pos_to_nt;// = {{0, 'A'},  {1, 'C'},  {2, 'T'}, {3, 'G'}, {4, 'D'}};

	string contig;
//	cerr << name;
	MappedSamStream sm;
//
//	while (!sm.eof()) {
//	SingleSamRead tmp;
//	sm >>tmp;
	//print tmp.
	vector<position_description> charts;

public:
	ContigProcessor(string sam_file, string contig_file):sam_file(sam_file), contig_file(contig_file),sm(sam_file){
		read_contig();
//		const map<char, char> nt_to_pos = {{'a', 0}, {'A', 0}, {'c', 1}, {'C', 1}, {'t', 2}, {'T', 2}, {'g', 3}, {'G', 3}, {'D', 4}};
//		const map< char, char> pos_to_nt = {{0, 'A'},  {1, 'C'},  {2, 'T'}, {3, 'G'}, {4, 'D'}};
	}
	void read_contig() {
		io::FileReadStream contig_stream(contig_file);
		io::SingleRead ctg;
		contig_stream >> ctg;
		contig = ctg.sequence().str();
		INFO("processing contig length " + contig.length());
		charts.resize(contig.length());
	}

	void UpdateOneRead(SingleSamRead &tmp){
		map<size_t, position_description> all_positions;
		tmp.CountPositions(all_positions);
		for (auto iter = all_positions.begin(); iter != all_positions.end(); ++iter) {
			charts[iter->first].update(iter->second);
		}
	}
//returns: number of changed nucleotides;
	int UpdateOneBase(size_t i, stringstream &ss){
		char old = toupper(contig[i]);
		size_t maxi = 0;
		int maxx = 0;
		for (size_t j = 0; j < max_votes; j++) {
			if (maxx < charts[i].votes[j]) {
				maxx = charts[i].votes[j];
				maxi = j;
			}
		}
		if (old != pos_to_nt.find(maxi)->second) {
			INFO("On position " << i << " changing " << old <<" to "<<pos_to_nt.find(maxi)->second);
			INFO(charts[i].str());
			if (maxi < 4) {
				ss <<pos_to_nt.find(maxi)->second;
				return 1;
			} else if (maxi == DELETION) {

				return 1;
			} else if (maxi == INSERTION) {
				string maxj = "";
				int max_ins = 0;
				for (auto iter = charts[i].insertions.begin(); iter != charts[i].insertions.end(); ++iter) {
					if (iter->second > max_ins){
						max_ins = iter->second;
						maxj = iter->first;
					}
				}
				INFO("most popular insertion: " << maxj);
				ss << maxj;
				if (old == maxj[0]) {
					return maxj.length() - 1;
				} else {
					return maxj.length();
				}


			}
		} else {
			ss << old;
			return 0;
		}
	}

	void process_sam_file (){
		while (!sm.eof()) {
			SingleSamRead tmp;
			sm >> tmp;
			UpdateOneRead(tmp);
			//	sm >>tmp;
		}
		stringstream s_new_contig;
		for (size_t i = 0; i < contig.length(); i ++) {
			UpdateOneBase(i, s_new_contig);
		}
		INFO (s_new_contig.str());
	}

	//string seq, cigar; pair<size_t, size_t> borders;
};