/*
  Copyright (c) 2006 - 2018
  CLST  - Radboud University
  ILK   - Tilburg University

  This file is part of ticcltools

  ticcltools is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 3 of the License, or
  (at your option) any later version.

  ticcltools is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, see <http://www.gnu.org/licenses/>.

  For questions and suggestions, see:
      https://github.com/LanguageMachines/ticcltools/issues
  or send mail to:
      lamasoftware (at ) science.ru.nl

*/

#include <unistd.h>
#include <set>
#include <map>
#include <limits>
#include <vector>
#include <cstdlib>
#include <string>
#include <stdexcept>
#include <iostream>
#include <fstream>
#include "config.h"
#ifdef HAVE_OPENMP
#include "omp.h"
#endif
#include "ticcutils/StringOps.h"
#include "ticcutils/CommandLine.h"
#include "ticcutils/PrettyPrint.h"
#include "ticcutils/Unicode.h"
#include "ticcl/unicode.h"
#include "config.h"

using namespace std;
using icu::UnicodeString;

typedef signed long int bitType;

string progname;
int verbose = 0;

void usage( const string& progname ){
  cerr << "usage: " << progname << endl;
  cerr << "\t--index <confuslist> as produced by TICCL-indexer or TICCL-indexerNT." << endl;
  cerr << "\t--hash <anahash>, as produced by TICCl-anahash," << endl;
  cerr << "\t--clean <cleanfile> as produced by TICCL-unk" << endl;
  cerr << "\t--diac <diacritics file> a list of 'diacritical' confusions." << endl;
  cerr << "\t--hist <historicalfile> a list of 'historical' confusions." << endl;
  cerr << "\t--alph <alphabet> an alphabet file (as produced by TICCL-lexstat)" << endl;
  cerr << "\t--nohld ignore --LD for 'historical' confusions." << endl;
  cerr << "\t-o <outputfile>" << endl;
  cerr << "\t-t <threads>\n\t--threads <threads> Number of threads to run on." << endl;
  cerr << "\t\t\t If 'threads' has the value \"max\", the number of threads is set to a" << endl;
  cerr << "\t\t\t reasonable value. (OMP_NUM_TREADS - 2)" << endl;
  cerr << "\t--LD <distance> The Levensthein (or edit) distance to use" << endl;
  cerr << "\t--artifrq <artifreq> " << endl;
  cerr << "\t-h or --help this message " << endl;
  cerr << "\t-v be verbose, repeat to be more verbose " << endl;
  cerr << "\t-V or --version show version " << endl;
  exit( EXIT_FAILURE );
}

bitType high_five( int val ){
  bitType result = val;
  result *= val;
  result *= val;
  result *= val;
  result *= val;
  return result;
}

unsigned int ldCompare( const UnicodeString& s1, const UnicodeString& s2 ){
  const size_t len1 = s1.length(), len2 = s2.length();
  vector<unsigned int> col(len2+1), prevCol(len2+1);
  for ( unsigned int i = 0; i < prevCol.size(); ++i ){
    prevCol[i] = i;
  }
  for ( unsigned int i = 0; i < len1; ++i ) {
    col[0] = i+1;
    for ( unsigned int j = 0; j < len2; ++j )
      col[j+1] = min( min( 1 + col[j], 1 + prevCol[1 + j]),
		      prevCol[j] + (s1[i]==s2[j] ? 0 : 1) );
    col.swap(prevCol);
  }
  unsigned int result = prevCol[len2];
  return result;
}

bool isClean( const UnicodeString& us, const set<UChar>& alfabet ){
  if ( alfabet.empty() )
    return true;
  for ( int i=0; i < us.length(); ++i ){
    if ( alfabet.find( us[i] ) == alfabet.end() )
      return false;
  }
  return true;
}

const UChar SEPARATOR = '_';

vector<UnicodeString> split( const UnicodeString& in, UChar symbol ){
  vector<UnicodeString> results;
  int  pos = 0;
  while ( pos >= 0 && pos < in.length() ){
    UnicodeString res;
    int p = in.indexOf( symbol, pos );
    if ( p < 0 ){
      res = in.tempSubString( pos );
      pos = p;
    }
    else {
      res = in.tempSubString( pos, p - pos );
      pos = p + 1;
    }
    if ( !res.isEmpty() ){
      results.push_back( res );
    }
  }
  return results;
}

int analyze_ngrams( const UnicodeString& us1,
		    const UnicodeString& us2,
		    const map<UnicodeString, size_t>& low_freqMap,
		    size_t freqTreshold,
		    int verbose,
		    map<UnicodeString,set<UnicodeString>>& dis_map,
		    map<UnicodeString, size_t>& dis_count ){
  vector<UnicodeString> parts1 = split( us1, SEPARATOR );
  vector<UnicodeString> parts2 = split( us2, SEPARATOR );
  if ( parts1.size() == 1 || parts1.size() != parts2.size() ){
    return 0; // nothing special
  }
  UnicodeString diff_part1;
  UnicodeString diff_part2;
  //
  // search for a pair of 'uncommon' parts in the 2 ngrams.
  for ( size_t i=0; i < parts1.size(); ++i ){
    if ( parts1[i] == parts2[i] ){
      // ok
    }
    else if ( diff_part1.isEmpty() ) {
      diff_part1 = parts1[i];
      diff_part2 = parts2[i];
    }
    else {
      return 0; // nothing special
    }
  }
  if ( diff_part1.isEmpty() ) {
    // can this happen?
    // anyway: nothing to do
    return 0; // nothing special
  }
  //
  // Ok, so we have a pair
  //
  UnicodeString lp1 = diff_part1;
  lp1.toLower();
  auto const& entry = low_freqMap.find( lp1 );
  if ( entry != low_freqMap.end()
       && entry->second >= freqTreshold ){
    // OK a high frequent word. translating probably won't do any good
    return 0; // nothing special
  }
  if ( verbose > 1 ){
#pragma omp critical (debugout)
    {
      cerr << "check candidate: " << diff_part1
	   << " in n-grams pair: " << us1 << " # " << us2 << endl;
    }
  }
  if ( diff_part1.length() < 6 ){
    // a 'short' word
    UnicodeString disamb_pair = diff_part1 + "~" + diff_part2;
    // count this short words pair ANS store the original n-gram pair
#pragma omp critical (update)
    {
      dis_map[disamb_pair].insert( us1 + "~" + us2 );
      ++dis_count[disamb_pair];
    }
  }
  return 1; // signal a point
}

void handleTranspositions( ostream& os, const set<string>& s,
			   const map<string,size_t>& freqMap,
			   const map<UnicodeString,size_t>& low_freqMap,
			   const set<UChar>& alfabet,
			   map<UnicodeString,set<UnicodeString>>& dis_map,
			   map<UnicodeString, size_t>& dis_count,
			   size_t freqTreshold,
			   bool isKHC,
			   bool noKHCld,
			   bool isDIAC ){
  set<string>::const_iterator it1 = s.begin();
  while ( it1 != s.end() ) {
    int local_verbose = verbose;
    string str1 = *it1;
    // if ( str1 == "noodlyk" || str1 == "noorderlyke" ){
    //   local_verbose = 3;
    // }
    if ( local_verbose > 2 ){
#pragma omp critical (debugout)
      {
	cout << "TRANSPOSE: string 1 " << str1 << endl;
      }
    }
    map<string,size_t>::const_iterator fit = freqMap.find( str1 );
    if ( fit == freqMap.end() ){
      if ( local_verbose > 1 ){
#pragma omp critical (debugout)
	{
	  cout << "not found in freq file " << str1 << endl;
	}
      }
      ++it1;
      continue;
    }
    size_t freq1 = fit->second;
    set<string>::const_iterator it2 = it1;
    ++it2;
    while ( it2 != s.end() ) {
      string str2 = *it2;
      // if ( str2 == "noodlyk" || str2 == "noorderlyke" ){
      // 	local_verbose = 3;
      // }
      if ( local_verbose > 2 ){
#pragma omp critical (debugout)
	{
	  cout << "TRANSPOSE string 2 " << str2 << endl;
	}
      }
      map<string,size_t>::const_iterator fit = freqMap.find( str2 );
      if ( fit == freqMap.end() ){
	if ( local_verbose > 1 ){
#pragma omp critical (debugout)
	  {
	    cout << "not found in freq file " << str2 << endl;
	  }
	}
	++it2;
	continue;
      }
      size_t freq2 = fit->second;
      UnicodeString us1 = TiCC::UnicodeFromUTF8( str1 );
      UnicodeString ls1 = us1;
      ls1.toLower();
      UnicodeString us2 = TiCC::UnicodeFromUTF8( str2 );
      UnicodeString ls2 = us2;
      ls2.toLower();

      size_t out_freq1;
      size_t out_low_freq1;
      size_t out_freq2;
      size_t out_low_freq2;
      string out_str1;
      string out_str2;
      size_t low_freq1 = low_freqMap.at(ls1);
      size_t low_freq2 = low_freqMap.at(ls2);
      if ( low_freq1 >= freqTreshold && low_freq2 >= freqTreshold
	   && !isDIAC ){
	++it2;
	continue;
      }
      if ( low_freq1 >= low_freq2 ){
	if ( low_freq1 < freqTreshold ){
	  ++it2;
	  continue;
	}
      }
      else {
	if ( low_freq2 < freqTreshold ){
	  ++it2;
	  continue;
	}
      }

      size_t canon_freq = 0;
      UnicodeString candidate;
      bool swapped = false;
      if ( low_freq1 > low_freq2 ){
	canon_freq = low_freq1;
	out_freq1 = freq2;
	out_low_freq1 = low_freq2;
	out_freq2 = freq1;
	out_low_freq2 = low_freq1;
	out_str1 = str2;
	out_str2 = str1;
	candidate = ls1;
	swapped = true;
      }
      else {
	canon_freq = low_freq2;
	out_freq1 = freq1;
	out_low_freq1 = low_freq1;
	out_freq2 = freq2;
	out_low_freq2 = low_freq2;
	out_str1 = str1;
	out_str2 = str2;
	candidate = ls2;
      }
      if ( !isClean( candidate, alfabet ) ){
	if ( local_verbose > 1 ){
#pragma omp critical (debugout)
	  {
	    cout << "ignore dirty candidate " << candidate << endl;
	  }
	}
	++it2;
	continue;
      }
      int ngram_point = 0;
      if ( swapped ){
	ngram_point = analyze_ngrams( us2, us1, low_freqMap, freqTreshold,
				      verbose, dis_map, dis_count );
      }
      else {
	ngram_point = analyze_ngrams( us1, us2, low_freqMap, freqTreshold,
				      verbose, dis_map, dis_count );
      }
      unsigned int ld = ldCompare( ls1, ls2 );
      if ( ld != 2 ){
	if ( !( isKHC && noKHCld ) ){
	  if ( local_verbose > 1 ){
#pragma omp critical (debugout)
	    {
	      cout << " LD != 2 " << str1 << "," << str2 << endl;
	    }
	  }
	  ++it2;
	  continue;
	}
      }

      int cls = max(ls1.length(),ls2.length()) - ld;
      string canon = "0";
      if ( canon_freq >= freqTreshold ){
	canon = "1";
      }
      string FLoverlap = "0";
      if ( ls1[0] == ls2[0] ){
	FLoverlap = "1";
      }
      string LLoverlap = "0";
      if ( ls1.length() > 1 && ls2.length() > 1
	   && ls1[ls1.length()-1] == ls2[ls2.length()-1]
	   && ls1[ls1.length()-2] == ls2[ls2.length()-2] ){
	LLoverlap = "1";
      }
      string KHC = "0";
      if ( isKHC ){
	KHC = "1";
      }
      string result = out_str1 + "~" + TiCC::toString(out_freq1) + "~"
	+ TiCC::toString(out_low_freq1) + "~"
	+ out_str2 + "~" + TiCC::toString( out_freq2 ) + "~"
	+ TiCC::toString(out_low_freq2) + "~"
	+ "~0~" + TiCC::toString( ld ) + "~"
	+ TiCC::toString(cls) + "~" + canon + "~"
	+ FLoverlap + "~" + LLoverlap + "~"
	+ KHC + "~" + TiCC::toString(ngram_point);
#pragma omp critical (output)
      {
	os << result << endl;
      }
      if ( local_verbose > 2 ){
	cerr << "Transpose result: " << result << endl;
      }
      ++it2;
    }
    ++it1;
  }
}

void compareSets( ostream& os, unsigned int ldValue,
		  const string& KWC,
		  const set<string>& s1, const set<string>& s2,
		  const map<string,size_t>& freqMap,
		  const map<UnicodeString,size_t>& low_freqMap,
		  const set<UChar>& alfabet,
		  map<UnicodeString,set<UnicodeString>>& dis_map,
		  map<UnicodeString, size_t>& dis_count,
		  size_t freqTreshold,
		  bool isKHC,
		  bool noKHCld,
		  bool isDIAC ){
  // using TiCC::operator<<;
  // cerr << "set 1 " << s1 << endl;
  // cerr << "set 2 " << s2 << endl;
  set<string>::const_iterator it1 = s1.begin();
  while ( it1 != s1.end() ) {
    int local_verbose = verbose;
    string str1 = *it1;
    // if ( str1 == "noodlyk" || str1 == "noorderlyke" ){
    //   local_verbose = 3;
    // }
    if ( local_verbose > 2 ){
#pragma omp critical (debugout)
      {
	cout << "SET: string 1 " << str1 << endl;
      }
    }
    map<string,size_t>::const_iterator fit = freqMap.find( str1 );
    if ( fit == freqMap.end() ){
      if ( local_verbose > 1 ){
#pragma omp critical (debugout)
	{
	  cout << "not found in freq file " << str1 << endl;
	}
      }
      ++it1;
      continue;
    }
    size_t freq1 = fit->second;
    UnicodeString us1 = TiCC::UnicodeFromUTF8( str1 );
    UnicodeString ls1 = us1;
    ls1.toLower();
    set<string>::const_iterator it2 = s2.begin();
    while ( it2 != s2.end() ) {
      string str2 = *it2;
      // if ( str2 == "noodlyk" || str2 == "noorderlyke" ){
      // 	local_verbose = 3;
      // }
      if ( local_verbose > 2 ){
#pragma omp critical (debugout)
	{
	  cout << "SET: string 2 " << str2 << endl;
	}
      }
      fit = freqMap.find( str2 );
      if ( fit == freqMap.end() ){
	if ( local_verbose > 1 ){
#pragma omp critical (debugout)
	  {
	    cout << "not found in freq file " << str2 << endl;
	  }
	}
	++it2;
	continue;
      }

      size_t freq2 = fit->second;
      UnicodeString us2 = TiCC::UnicodeFromUTF8( str2 );
      UnicodeString ls2 = us2;
      ls2.toLower();
      unsigned int ld = ldCompare( ls1, ls2 );
      if ( ld > ldValue ){
	if ( !( isKHC && noKHCld ) ){
	  if ( local_verbose > 2 ){
#pragma omp critical (debugout)
	    {
	      cout << " LD too high " << str1 << "," << str2 << endl;
	    }
	  }
	  ++it2;
	  continue;
	}
      }

      size_t out_freq1;
      size_t out_low_freq1;
      size_t out_freq2;
      size_t out_low_freq2;
      string out_str1;
      string out_str2;
      size_t low_freq1 = low_freqMap.at(ls1);
      size_t low_freq2 = low_freqMap.at(ls2);
      size_t canon_freq = 0;
      UnicodeString candidate;
      bool swapped = false;
      if ( low_freq1 > low_freq2 ){
	canon_freq = low_freq1;
	out_freq1 = freq2;
	out_low_freq1 = low_freq2;
	out_freq2 = freq1;
	out_low_freq2 = low_freq1;
	out_str1 = str2;
	out_str2 = str1;
	candidate = ls1;
	swapped = true;
      }
      else {
	canon_freq = low_freq2;
	out_freq1 = freq1;
	out_low_freq1 = low_freq1;
	out_freq2 = freq2;
	out_low_freq2 = low_freq2;
	out_str1 = str1;
	out_str2 = str2;
	candidate = ls2;
      }
      if ( !isClean( candidate, alfabet ) ){
	if ( local_verbose > 1 ){
#pragma omp critical (debugout)
	  {
	    cout << "ignore dirty candidate " << candidate << endl;
	  }
	}
	++it2;
	continue;
      }
      if ( out_low_freq1 >= freqTreshold && !isDIAC ){
	if ( local_verbose > 2 ){
#pragma omp critical (debugout)
	  {
	    cout << "lexical word " << out_str1 << endl;
	  }
	}
	++it2;
	continue;
      }
      int ngram_point = 0;
      if ( swapped ){
	ngram_point = analyze_ngrams( us2, us1, low_freqMap, freqTreshold,
				      verbose, dis_map, dis_count );
      }
      else {
	ngram_point = analyze_ngrams( us1, us2, low_freqMap, freqTreshold,
				      verbose, dis_map, dis_count );
      }

      int cls = max(ls1.length(),ls2.length()) - ld;
      string canon = "0";
      if ( canon_freq >= freqTreshold ){
	canon = "1";
      }
      string FLoverlap = "0";
      if ( ls1[0] == ls2[0] ){
	FLoverlap = "1";
      }
      string LLoverlap = "0";
      if ( ls1.length() > 1 && ls2.length() > 1
	   && ls1[ls1.length()-1] == ls2[ls2.length()-1]
	   && ls1[ls1.length()-2] == ls2[ls2.length()-2] ){
	LLoverlap = "1";
      }
      string KHC = "0";
      if ( isKHC ){
	KHC = "1";
      }
      string result = out_str1 + "~" + TiCC::toString(out_freq1) +
	+ "~" + TiCC::toString(out_low_freq1) +
	+ "~" + out_str2 + "~" + TiCC::toString( out_freq2 )
	+ "~" + TiCC::toString( out_low_freq2 )
	+ "~" + KWC + "~" + TiCC::toString( ld ) + "~"
	+ TiCC::toString(cls) + "~" + canon + "~"
	+ FLoverlap + "~" + LLoverlap + "~"
	+ KHC + "~" + TiCC::toString(ngram_point);
#pragma omp critical (output)
      {
	os << result << endl;
      }
      if ( local_verbose > 2 ){
	cerr << "SET result: " << result << endl;
      }
      ++it2;
    }
    ++it1;
  }
}

void add_ambi( ostream& os,
	       const map<UnicodeString,size_t>& dis_count,
	       const map<string,size_t>& freqMap,
	       const map<UnicodeString,size_t>& low_freqMap ){
  for ( const auto& entry : dis_count ){
    vector<UnicodeString> parts = TiCC::split_at( entry.first, "~" );
    int ld = ldCompare( parts[0], parts[1] );
    int cls = max(parts[0].length(),parts[1].length()) - ld;
    string FLoverlap = "0";
    if ( parts[0][0] == parts[1][0] ){
      FLoverlap = "1";
    }
    string LLoverlap = "0";
    if ( parts[0].length() > 1 && parts[1].length() > 1
	 && parts[0][parts[0].length()-1] == parts[1][parts[1].length()-1]
	 && parts[0][parts[0].length()-2] == parts[1][parts[1].length()-2] ){
      LLoverlap = "1";
    }
    size_t freq1 = 0;
    auto it = freqMap.find(TiCC::UnicodeToUTF8(parts[0]));
    if ( it != freqMap.end() ){
      freq1 = it->second;
    }
    size_t freq2 = 0;
    it = freqMap.find(TiCC::UnicodeToUTF8(parts[1]));
    if ( it != freqMap.end() ){
      freq2 = it->second;
    }
    size_t low_freq1 = 0;
    auto it2 = low_freqMap.find(parts[0]);
    if ( it2 != low_freqMap.end() ){
      low_freq1 = it2->second;
    }
    size_t low_freq2 = 0;
    it2 = low_freqMap.find(parts[1]);
    if ( it2 != low_freqMap.end() ){
      low_freq2 = it2->second;
    }
    os << parts[0] << "~" << freq1
       << "~" << low_freq1
       << "~" << parts[1] << "~" << freq2
       << "~" << low_freq2
       << "~0~" << ld
       << "~" << cls
       << "~" << 0
       << "~" << FLoverlap << "~" << LLoverlap
       << "~0~" << entry.second << endl;
  }
}

int main( int argc, char **argv ){
  // UnicodeString s1 = "Een_Test";
  // UnicodeString s2 = "een_wat_langere_Tast";
  // vector<UnicodeString> bla = split( s1, SEPARATOR );
  // for ( const auto& s : bla ){
  //   cerr << s << endl;
  // }
  // bla = split( s2, SEPARATOR );
  // for ( const auto& s : bla ){
  //   cerr << s << endl;
  // }
  // exit(EXIT_SUCCESS);
  TiCC::CL_Options opts;
  try {
    opts.set_short_options( "vVho:t:" );
    opts.set_long_options( "diac:,hist:,nohld,artifrq:,LD:,hash:,clean:,"
			   "alph:,index:,help,version,threads:" );
    opts.init( argc, argv );
  }
  catch( TiCC::OptionError& e ){
    progname = opts.prog_name();
    cerr << e.what() << endl;
    usage( progname );
    exit( EXIT_FAILURE );
  }
  progname = opts.prog_name();
  if ( argc < 2	){
    usage( progname );
    exit(EXIT_FAILURE);
  }
  if ( opts.extract('h') || opts.extract("help") ){
    usage( progname );
    exit(EXIT_SUCCESS);
  }
  if ( opts.extract('V') || opts.extract("version") ){
    cerr << progname << ": " << PACKAGE_STRING << endl;
    exit(EXIT_SUCCESS);
  }
  while ( opts.extract( 'v' ) ){
    ++verbose;
  }

  string indexFile;
  string anahashFile;
  string frequencyFile;
  string histconfFile;
  string diaconfFile;
  string alfabetFile;
  int LDvalue=2;
  bool noKHCld = opts.extract("nohld");
  if ( !opts.extract( "index", indexFile ) ){
    cerr << progname << ": missing --index option" << endl;
    exit( EXIT_FAILURE );
  }
  if ( !TiCC::match_back( indexFile, ".index" )
       && !TiCC::match_back( indexFile, ".indexNT" ) ){
    cerr << progname << ": --index files must have extension: '.index' or '.indexNT' "
	 << endl;
    exit( EXIT_FAILURE );
  }
  if ( !opts.extract( "hash", anahashFile ) ){
    cerr << progname << ": missing --hash option" << endl;
    exit( EXIT_FAILURE );
  }
  if ( !opts.extract( "clean", frequencyFile ) ){
    cerr << progname << ": missing --clean option" << endl;
    exit( EXIT_FAILURE );
  }
  opts.extract( "alph", alfabetFile );
  opts.extract( "hist", histconfFile );
  if ( opts.extract( "diac", diaconfFile ) ){
    if ( !TiCC::match_back( diaconfFile, ".diac" ) ){
      cerr << progname << ": invalid extension for --diac file '" << diaconfFile
	   << "' (must be .diac) " << endl;
      exit(EXIT_FAILURE);
    }
  }
  string outFile;
  if ( opts.extract( 'o', outFile ) ){
    if ( !TiCC::match_back( outFile, ".ldcalc" ) )
      outFile += ".ldcalc";
  }
  else {
    outFile = indexFile + ".ldcalc";
  }
  string ambiFile = outFile + ".ambi";
  size_t artifreq = 0;
  string value;

  if ( opts.extract( "artifrq", value ) ){
    if ( !TiCC::stringTo(value,artifreq) ) {
      cerr << progname << ": illegal value for --artifrq (" << value << ")" << endl;
      exit( EXIT_FAILURE );
    }
  }
  value = "1";
  if ( !opts.extract( 't', value ) ){
    opts.extract( "threads", value );
  }
#ifdef HAVE_OPENMP
  int numThreads;
  if ( TiCC::lowercase(value) == "max" ){
    numThreads = omp_get_max_threads() - 2;
    omp_set_num_threads( numThreads );
    cout << "running on " << numThreads << " threads." << endl;
  }
  else {
    if ( !TiCC::stringTo(value,numThreads) ) {
      cerr << "illegal value for -t (" << value << ")" << endl;
      exit( EXIT_FAILURE );
    }
    omp_set_num_threads( numThreads );
    cout << "running on " << numThreads << " threads." << endl;
  }
#else
  if ( value != "1" ){
    cerr << "unable to set number of threads!.\nNo OpenMP support available!"
	 <<endl;
    exit(EXIT_FAILURE);
  }
#endif
  if ( opts.extract( "LD", value ) ){
    if ( !TiCC::stringTo(value,LDvalue) ) {
      cerr << progname << ": illegal value for --LD (" << value << ")" << endl;
      exit( EXIT_FAILURE );
    }
    if ( LDvalue < 1 || LDvalue > 10 ){
      cerr << progname << ": invalid LD value: " << LDvalue << " (1-10 is OK)" << endl;
      exit( EXIT_FAILURE );
    }
  }
  if ( !opts.empty() ){
    cerr << progname << ": unsupported options : " << opts.toString() << endl;
    usage(progname);
    exit(EXIT_FAILURE);
  }

  set<UChar> alfabet;
  if ( !alfabetFile.empty() ){
    ifstream lexicon( alfabetFile );
    if ( !lexicon ){
      cerr << progname << ": problem opening alfabet file: " << alfabetFile << endl;
      exit(EXIT_FAILURE);
    }
    cout << progname << ": reading alphabet: " << alfabetFile << endl;
    string line;
    while ( getline( lexicon, line ) ){
      if ( line.size() == 0 || line[0] == '#' )
	continue;
      vector<string> vec;
      if ( TiCC::split( line, vec ) != 3 ){
	cerr << progname << ": invalid line '" << line << "' in " << alfabetFile << endl;
	exit( EXIT_FAILURE );
      }
      UnicodeString key = TiCC::UnicodeFromUTF8(vec[0]);
      alfabet.insert(key[0]);
    }
  }
  cout << progname << ": read " << alfabet.size() << " letters with frequencies" << endl;

  ifstream ff( frequencyFile  );
  if ( !ff ){
    cerr << progname << ": problem opening " << frequencyFile << endl;
    exit(EXIT_FAILURE);
  }
  cout << progname << ": reading clean file: " << frequencyFile << endl;
  map<string, size_t> freqMap;
  map<UnicodeString, size_t> low_freqMap;
  string line;
  size_t ign = 0;
  while ( getline( ff, line ) ){
    vector<string> v1;
    if ( TiCC::split( line, v1 ) != 2 ){
      ++ign;
      continue;
    }
    else {
      string s = v1[0];
      size_t freq = TiCC::stringTo<size_t>( v1[1] );
      freqMap[s] = freq;
      UnicodeString ls = TiCC::UnicodeFromUTF8( s );
      ls.toLower();
      if ( freq >= artifreq ){
	// make sure that the artifrq is counted only once!
	if ( low_freqMap[ls] == 0 ){
	  low_freqMap[ls] = freq;
	}
	else {
	  low_freqMap[ls] += freq-artifreq;
	}
      }
      else {
	low_freqMap[ls] +=freq;
      }
    }
  }
  cout << progname << ": read " << freqMap.size() << " clean words with frequencies" << endl;
  cout << progname << ": skipped " << ign << " n-grams" << endl;

  set<bitType> histMap;
  if ( !histconfFile.empty() ){
    ifstream ff( histconfFile );
    if ( !ff ){
      cerr << "problem opening " << histconfFile << endl;
      exit(EXIT_FAILURE);
    }
    string line;
    while ( getline( ff, line ) ){
      vector<string> v;
      if ( TiCC::split_at( line, v, "#" ) < 2
	   || TiCC::split_at( line, v, "#" ) > 3 ){
	continue;
      }
      bitType val = TiCC::stringTo<bitType>(v[0]);
      histMap.insert(val);
    }
    if ( histMap.empty() ){
      cerr << progname << ": the historical confusions file " << histconfFile
	   << " doesn't seem to be in the right format." << endl
	   << " should contain lines like: 10331739614#f~s" << endl;
    }
    else {
      cout << progname << ": read " << histMap.size() << " historical confusions." << endl;
    }
  }

  set<bitType> diaMap;
  if ( !diaconfFile.empty() ){
    ifstream ff( diaconfFile );
    if ( !ff ){
      cerr << progname << ": problem opening " << diaconfFile << endl;
      exit(EXIT_FAILURE);
    }
    string line;
    while ( getline( ff, line ) ){
      vector<string> v;
      if ( TiCC::split_at( line, v, "#" ) < 2
	   || TiCC::split_at( line, v, "#" ) > 3 ){
	continue;
      }
      bitType val = TiCC::stringTo<bitType>(v[0]);
      diaMap.insert(val);
    }
    if ( diaMap.empty() ){
      cerr << progname << ": the diacritical confusions file " << histconfFile
	   << " doesn't seem to be in the right format." << endl
	   << " should contain lines like: 10331739614#e~é" << endl;
      exit(EXIT_FAILURE);
    }
    else {
      cout << progname << ": read " << diaMap.size() << " diacritical confusions." << endl;
    }
  }

  ifstream indexf( indexFile );
  if ( !indexf ){
    cerr << progname << ": problem opening: " << indexFile << endl;
    exit(EXIT_FAILURE);
  }
  ifstream anaf( anahashFile );
  if ( !anaf ){
    cerr << progname << ": problem opening anagram hashes file: " << anahashFile << endl;
    exit(EXIT_FAILURE);
  }
  map<bitType,set<string> > hashMap;
  while ( getline( anaf, line ) ){
    vector<string> v1;
    if ( TiCC::split_at( line, v1, "~" ) != 2 )
      continue;
    else {
      vector<string> v2;
      if ( TiCC::split_at( v1[1], v2, "#" ) < 1 ){
	cerr << progname << ": strange line: " << line << endl
	     << " in anagram hashes file" << endl;
	exit(EXIT_FAILURE);
      }
      else {
	bitType key = TiCC::stringTo<bitType>( v1[0] );
	for ( size_t i=0; i < v2.size(); ++i )
	  hashMap[key].insert( v2[i] );
      }
    }
  }
  cout << progname << ": read " << hashMap.size() << " hash values" << endl;

  size_t count=0;
  ofstream os( outFile );
  set<bitType> handledTrans;
  map<UnicodeString,set<UnicodeString>> dis_map;
  map<UnicodeString,size_t> dis_count;
  size_t line_nr = 0;
  int err_cnt = 0;
  while ( getline( indexf, line ) ){
    if ( err_cnt > 9 ){
      cerr << progname << ": FATAL ERROR: too many problems in indexfile: " << indexFile
	   << " terminated" << endl;
      exit( EXIT_FAILURE);
    }
    ++line_nr;
    if ( verbose > 1 ){
      cerr << "examine " << line << endl;
    }
    line = TiCC::trim(line);
    if ( line.empty() ){
      continue;
    }
    vector<string> parts;
    if ( TiCC::split_at( line, parts, "#" ) != 2 ){
      cerr << progname << ": ERROR in line " << line_nr
	   << " of indexfile: unable to split in 2 parts at #"
	   << endl << "line was" << endl << line << endl;
      ++err_cnt;
    }
    else {
      string mainKeyS = parts[0];
      if ( ++count % 1000 == 0 ){
	cout << ".";
	cout.flush();
	if ( count % 50000 == 0 ){
	  cout << endl << count << endl;;
	}
      }
      string rest = parts[1];
      if ( verbose > 1 ){
	cerr << "extract parts from " << rest << endl;
      }
      if ( TiCC::split_at( rest, parts, "," ) < 1 ){
	cerr << progname << ": ERROR in line " << line_nr
	     << " of indexfile: unable to split in parts separated by ','"
	     << endl << "line was" << endl << line << endl;
	++err_cnt;
      }
      else {
	bitType mainKey = TiCC::stringTo<bitType>(mainKeyS);
	bool isKHC = false;
	if ( histMap.find( mainKey ) != histMap.end() ){
	  isKHC = true;
	}
	bool isDIAC = false;
	if ( diaMap.find( mainKey ) != diaMap.end() ){
	  isDIAC = true;
	}
#pragma omp parallel for schedule(dynamic,1)
	for ( size_t i=0; i < parts.size(); ++i ){
	  string keyS = parts[i];
	  bitType key = TiCC::stringTo<bitType>(keyS);
	  if ( verbose > 1 ){
#pragma omp critical (debugout)
	    cout << "bekijk key1 " << key << endl;
	  }
	  map<bitType,set<string> >::const_iterator sit1 = hashMap.find(key);
	  if ( sit1 == hashMap.end() ){
#pragma omp critical (debugout)
	    cerr << progname << ": WARNING: found a key '" << key
		 << "' in the input that isn't present in the hashes." << endl;
	    continue;
	  }
	  if ( sit1->second.size() > 0
	       && LDvalue >= 2 ){
	    bool do_trans = false;
#pragma omp critical (debugout)
	    {
	      set<bitType>::const_iterator it = handledTrans.find( key );
	      if ( it == handledTrans.end() ){
		handledTrans.insert( key );
		do_trans = true;
	      }
	    }
	    if ( do_trans ){
	      handleTranspositions( os, sit1->second,
				    freqMap, low_freqMap, alfabet,
				    dis_map, dis_count,
				    artifreq, isKHC, noKHCld, isDIAC );
	    }
	  }
	  if ( verbose > 1 ){
#pragma omp critical (debugout)
	    cout << "bekijk key2 " << mainKey + key << endl;
	  }
	  map<bitType, set<string> >::const_iterator sit2 = hashMap.find(mainKey+key);
	  if ( sit2 == hashMap.end() ){
	    if ( verbose > 4 ){
#pragma omp critical (debugout)
	      cerr << progname << ": WARNING: found a key '" << key
		   << "' in the input that, when added to '" << mainKey
		   << "' isn't present in the hashes." << endl;
	    }
	    continue;
	  }
	  compareSets( os, LDvalue, mainKeyS,
		       sit1->second, sit2->second,
		       freqMap, low_freqMap, alfabet,
		       dis_map, dis_count,
		       artifreq, isKHC, noKHCld, isDIAC );
	}
      }
    }
  }
  add_ambi( os, dis_count, freqMap, low_freqMap );
  cout << endl << "creating .ambi file: " << ambiFile << endl;
  ofstream amb ( ambiFile );
  for ( const auto& ambi : dis_map ){
    amb << ambi.first << "#";
    for ( const auto& val : ambi.second ){
      amb << val << "#";
    }
    amb << endl;
  }
  cout << progname << ": Done" << endl;
}
