//==========================================================================
//  AIDA Detector description implementation for LCD
//--------------------------------------------------------------------------
// Copyright (C) Organisation europeenne pour la Recherche nucleaire (CERN)
// All rights reserved.
//
// For the licensing terms see $DD4hepINSTALL/LICENSE.
// For the list of contributors see $DD4hepINSTALL/doc/CREDITS.
//
// Author     : M.Frank
//
//==========================================================================
//
// DDDB is a detector description convention developed by the LHCb experiment.
// For further information concerning the DTD, please see:
// http://lhcb-comp.web.cern.ch/lhcb-comp/Frameworks/DetDesc/Documents/lhcbDtd.pdf
//
//==========================================================================

// Framework includes
#include "DD4hep/LCDD.h"
#include "DD4hep/Path.h"
#include "DD4hep/Printout.h"
#include "DD4hep/Factories.h"
#include "DD4hep/InstanceCount.h"
#include "DD4hep/AlignmentsCalculator.h"
#include "DD4hep/detail/AlignmentsInterna.h"
#include "DDCond/ConditionsSlice.h"

#include "DDDB/DDDBConversion.h"
#include "DDDB/DDDBConditionPrinter.h"

#include "TStatistic.h"
#include "TTimeStamp.h"

using namespace std;
using namespace DD4hep;
using namespace DD4hep::Conditions;
using Alignments::Delta;
using Alignments::Alignment;
using Alignments::AlignmentsCalculator;

/// Anonymous namespace for plugins
namespace  {

  /// DDDB Conditions analyser to select alignments.
  /**
   *   \author  M.Frank
   *   \version 1.0
   *   \date    31/03/2016
   *   \ingroup DD4HEP_DDDB
   */
  class AlignmentSelector  {
  public:
    typedef std::shared_ptr<ConditionsContent>   Content;
    typedef DDDB::ConditionPrinter               Printer;
    std::map<DetElement,Condition::itemkey_type> deltas;
    LCDD&                lcdd;
    string               name;
    PrintLevel           printLevel     = INFO;
    unsigned int         alignmentKey   = 0;
    unsigned int         alignDeltaKey  = 0;
    long                 installCount   = 0;
    long                 accessCount    = 0;
    TStatistic           load_stat, comp_stat;
    Content              content;
    Printer              printer;
    /// Initializing constructor
    AlignmentSelector(LCDD& l, PrintLevel p)
      : lcdd(l), name("DDDBAlignments"), printLevel(p),
        load_stat("Load"), comp_stat("Compute"), printer(0,"Alignments")
    {
      printer.printLevel = DEBUG;
      content.reset(new ConditionsContent());
      alignmentKey  = ConditionKey::itemCode("alignment");
      alignDeltaKey = ConditionKey::itemCode("alignment_delta");
    }

    /// Default destructor
    virtual ~AlignmentSelector()   {
      printout(INFO,name,"++ DDDB: Installed alignment calls:     %ld", installCount);
      printout(INFO,name,"++ DDDB: Number of accessed alignments: %ld", accessCount);
      content.reset();
    }

    /// Recursive alignment collector
    long collect(DetElement de, ConditionsSlice& slice, int level)    {
      char fmt[64];
      try  {
        DDDB::Catalog* cat = de.extension<DDDB::Catalog>();
        if ( !cat->condition.empty() )  {
          ConditionKey key(de, alignDeltaKey);
          Condition    cond = slice.pool->get(key.hash);
          if ( cond.isValid() )   {
            deltas.insert(make_pair(de,cond.item_key()));
            ++installCount;
          }
          else  {
            ::sprintf(fmt,"%03d %%-%ds %%-20s -> %%s",level+1,2*level+3);
            printout(printLevel,name,fmt,"","Alignment:    ", 
                     !cond.isValid() ? (cat->condition+"  !!!UNRESOLVED!!!").c_str() : cond.name());
          }
        }
      }
      catch(const exception& e)  {
        ::sprintf(fmt,"%03d %%-%ds %%s: %%-20s  WARNING: %%s",level+1,2*level+3);
        printout(ERROR,name, fmt, "", de.path().c_str(), "[NO CATALOG availible]", e.what());
      }
      catch(...)  {
        ::sprintf(fmt,"%03d %%-%ds %%s: WARNING: %%-20s",level+1,2*level+3);
        printout(ERROR,name, fmt, "", de.path().c_str(), "[NO CATALOG availible]");
      }
      const DetElement::Children& c = de.children();
      for (DetElement::Children::const_iterator i = c.begin(); i != c.end(); ++i)
        collect((*i).second, slice, level+1);
      return 1;
    }
    /// Initial collector call
    long collect(ConditionsManager manager, const IOV& iov)  {
      shared_ptr<ConditionsSlice> slice(new ConditionsSlice(manager,content));
      Conditions::fill_content(manager,*content,*iov.iovType);

      ConditionsManager::Result cres = manager.prepare(iov, *slice);
      printout(INFO,name,
               "++ DDDB: Initial prepare: %7ld conditions (S:%ld,L:%ld,C:%ld,M:%ld) for IOV:%-12s",
               cres.total(), cres.selected, cres.loaded, cres.computed, cres.missing, iov.str().c_str());
      long res = collect(lcdd.world(), *slice, 0);
      printout(INFO,name,"++ DDDB: Collected %ld deltas for re-alignment.",deltas.size());
      return res;
    }
    /// Compute dependent alignment conditions
    int computeDependencies(shared_ptr<ConditionsSlice>& slice,
                            ConditionsManager manager,
                            const IOV& iov,
                            bool access = false)
    {
      AlignmentsCalculator align;
      AlignmentsCalculator::Deltas align_deltas;
      slice.reset(new ConditionsSlice(manager,content));
      TTimeStamp acc_start;
      ConditionsManager::Result cres = manager.prepare(iov, *slice);
      TTimeStamp acc_stop;
      load_stat.Fill(acc_stop.AsDouble()-acc_start.AsDouble());
      printout(INFO,name,
               "++ DDDB: Prepared %7ld conditions (S:%ld,L:%ld,C:%ld,M:%ld) for IOV:%-12s",
               cres.total(), cres.selected, cres.loaded, cres.computed, cres.missing, iov.str().c_str());
      for ( const auto& e : deltas )  {
        Condition c = slice->get(e.first, e.second);
        try {
          //printer(c);
          if ( c->testFlag(Condition::ALIGNMENT_DELTA) )  {
#if 0
            const Delta& delta = c.get<Delta>();
#endif
            AbstractMap& d = c.get<AbstractMap>();
            const pair<string,OpaqueDataBlock>& block = *(d.params.begin());
            const Delta& delta = block.second.get<Delta>();            
            align_deltas.insert(make_pair(e.first, delta));
          }
          else {
            printout(WARNING,name,"++ DDDB: Ignore condition: %s -> %s",c.name(), c->value.c_str());
          }
        }
        catch(const exception& exc)  {
          printout(WARNING,name,"++ DDDB: Invalid alignment for %s: %s",e.first.path().c_str(),exc.what());
        }
      }
      TTimeStamp comp_start;
      AlignmentsCalculator::Result ares = align.compute(align_deltas,*slice);
      TTimeStamp comp_stop;
      comp_stat.Fill(comp_stop.AsDouble()-comp_start.AsDouble());
      printout(INFO,name,
               "++ DDDB: AlignmentManager: %7ld conditions (S:%ld,L:%ld,C:%ld,M:%ld) alignments: (A:%ld,M:%ld) for IOV:%-12s",
               cres.total(), cres.selected, cres.loaded, cres.computed, cres.missing, 
               ares.computed, ares.missing, iov.str().c_str());
      if ( access )   {
        int count = 0;
        set<DetElement> detectors;
        for ( const auto& e : deltas )  {
          Condition               c = slice->get(e.first,alignmentKey);
          Alignment               a = c;
          const Delta&            D = a.data().delta;
          printout(PrintLevel(printLevel+1),"Alignment",
                   "++ [%16llX] (%11s-%8s-%5s) Cond:%p '%s . %s'", c.key(),
                   D.hasTranslation() ? "Translation" : "",
                   D.hasRotation() ? "Rotation" : "",
                   D.hasPivot() ? "Pivot" : "",
                   c.ptr(),
                   e.first.path().c_str(),
                   c.name());
          detectors.insert(e.first);
          ++count;
          ++accessCount;
        }
        printout(INFO,name,"++ DDDB: Accessed %ld alignments from %ld DetElement objects.",
                 count, detectors.size());
      }
      return 1;
    }
  };
}

//==========================================================================
namespace  {

  /// Plugin function: Load dependent alignment conditions according to time stamps.
  long dddb_derived_alignments(LCDD& lcdd, int argc, char** argv) {
    int turns = 1, accesses = 1;
    PrintLevel level = INFO;
    long time = makeTime(2016,4,1,12);
    string fname = Path(__FILE__).filename();
    
    for(int i=0; i<argc; ++i)  {
      if ( ::strncmp(argv[i],"-time",3)==0 )  {
        time = makeTime(argv[++i],"%d-%m-%Y %H:%M:%S");
        printout(level,"DDDB","Setting event time in %s to %s [%ld]",
                 fname.c_str(), argv[i-1], time);
      }
      else if ( ::strncmp(argv[i],"-print",3)==0 )  {
        level = DD4hep::printLevel(argv[++i]);
        printout(level,"DDDB","Setting print level in %s to %s [%d]",
                 fname.c_str(), argv[i-1], level);
      }
      else if ( ::strncmp(argv[i],"-turns",3)==0 )  {
        turns = ::atol(argv[++i]);
        printout(level,"DDDB","Accumulate statistics in %s for %d [%s] turns.",
                 fname.c_str(), turns, argv[i-1]);
      }
      else if ( ::strncmp(argv[i],"-access",3)==0 )  {
        accesses = ::atol(argv[++i]);
        printout(level,"DDDB","Accumulate statistics in %s for %d [%s] accesses per turn.",
                 fname.c_str(), turns, argv[i-1]);
      }
      else if ( ::strcmp(argv[i],"--help")==0 )      {
        printout(level,"Plugin-Help","Usage: DDDB_DerivedAlignmentsTest --opt [--opt]        ");
        printout(level,"Plugin-Help","  -time  <string>     Set event time Format: \"%%d-%%m-%%Y %%H:%%M:%%S\"");
        printout(level,"Plugin-Help","  -print <value>      Printlevel for output      ");
        printout(level,"Plugin-Help","  -help               Print this help message    ");
        ::exit(EINVAL);
      }
    }
    int ret;
    {
      AlignmentSelector selec(lcdd, level);
      ConditionsManager manager(ConditionsManager::from(lcdd));
      TStatistic        cr_stat("Initialize"), re_acc_stat("Reaccess");
      const IOVType*    iovType = manager.iovType("epoch");
      {
        TTimeStamp start;
        IOV  iov(iovType, time);
        ret = selec.collect(manager,iov);
        TTimeStamp stop;
        cr_stat.Fill(stop.AsDouble()-start.AsDouble());
      }
      if ( ret == 1 )  {
        std::vector<IOV> iovs;
        for(int i=0; i<turns; ++i)  {
          shared_ptr<ConditionsSlice> slice;
          iovs.push_back(IOV(iovType, time + (i+1)*3600));
          ret = selec.computeDependencies(slice,manager,iovs.back());
        }
        
        for(int j = 0; j < accesses; ++j )   {
          for(int i = 0; i < turns; ++i )  {
            const IOV& iov = iovs[i];
            shared_ptr<ConditionsSlice> slice(new ConditionsSlice(manager,selec.content));
            TTimeStamp start;
            ConditionsManager::Result cres = manager.prepare(iov, *slice);
            TTimeStamp stop;
            re_acc_stat.Fill(stop.AsDouble()-start.AsDouble());
            printout(INFO,"DDDBAlign",
                     "++ DDDB: REACCESS:         %7ld conditions (S:%ld,L:%ld,C:%ld,M:%ld)             for IOV:%-12s",
                     cres.total(), cres.selected, cres.loaded, cres.computed, cres.missing, iov.str().c_str());
          }
        }
      }
      printout(INFO,"Statistics","+======= Summary: # of Runs: %3d ==========================================",turns);
      printout(INFO,"Statistics","+ DDDB:  %-12s:  %11.5g +- %11.4g  RMS = %11.5g  N = %lld",
               cr_stat.GetName(), cr_stat.GetMean(), cr_stat.GetMeanErr(), cr_stat.GetRMS(), cr_stat.GetN());
      printout(INFO,"Statistics","+ DDDB:  %-12s:  %11.5g +- %11.4g  RMS = %11.5g  N = %lld",
               selec.load_stat.GetName(), selec.load_stat.GetMean(), selec.load_stat.GetMeanErr(),
               selec.load_stat.GetRMS(), selec.load_stat.GetN());
      printout(INFO,"Statistics","+ DDDB:  %-12s:  %11.5g +- %11.4g  RMS = %11.5g  N = %lld",
               selec.comp_stat.GetName(), selec.comp_stat.GetMean(), selec.comp_stat.GetMeanErr(),
               selec.comp_stat.GetRMS(), selec.comp_stat.GetN());
      printout(INFO,"Statistics","+ DDDB:  %-12s:  %11.5g +- %11.4g  RMS = %11.5g  N = %lld",
               re_acc_stat.GetName(), re_acc_stat.GetMean(), re_acc_stat.GetMeanErr(),
               re_acc_stat.GetRMS(), re_acc_stat.GetN());
      printout(INFO,"Statistics","+=========================================================================");
    }
    InstanceCount::dump();
    return ret;
  }
}   /* End anonymous namespace  */
DECLARE_APPLY(DDDB_DerivedAlignmentsTest,dddb_derived_alignments)
//==========================================================================

namespace  {
  /// Plugin function: Access dependent alignment conditions from DetElement object using global and local keys
  long dddb_access_alignments(LCDD& lcdd, int argc, char** argv) {
    PrintLevel level = INFO;
    long time = makeTime(2016,4,1,12);
    for(int i=0; i<argc; ++i)  {
      if ( ::strcmp(argv[i],"-time")==0 )  {
        time = makeTime(argv[++i],"%d-%m-%Y %H:%M:%S");
        printout(level,"DDDB","Setting event time in %s to %s [%ld]",
                 Path(__FILE__).filename().c_str(), argv[i-1], time);
      }
      else if ( ::strcmp(argv[i],"-print")==0 )  {
        level = DD4hep::printLevel(argv[++i]);
        printout(level,"DDDB","Setting print level in %s to %s [%d]",
                 Path(__FILE__).filename().c_str(), argv[i-1], level);
      }
      else if ( ::strcmp(argv[i],"--help")==0 )      {
        printout(level,"Plugin-Help","Usage: DDDB_AlignmentsAccessTest --opt [--opt]        ");
        printout(level,"Plugin-Help","  -time  <string>     Set event time Format: \"%%d-%%m-%%Y %%H:%%M:%%S\"");
        printout(level,"Plugin-Help","  -print <value>      Printlevel for output      ");
        printout(level,"Plugin-Help","  -help               Print this help message    ");
        ::exit(EINVAL);
      }
    }
    AlignmentSelector selec(lcdd,level);
    ConditionsManager manager(ConditionsManager::from(lcdd));
    const IOVType* iovType = manager.iovType("epoch");
    IOV  iov(iovType, time);
    int ret = selec.collect(manager,iov);
    if ( ret == 1 )  {
      shared_ptr<ConditionsSlice> slice;
      ret = selec.computeDependencies(slice,manager,iov, true);
    }
    return ret;
  }
}   /* End anonymous namespace  */
DECLARE_APPLY(DDDB_AlignmentsAccessTest,dddb_access_alignments)
//==========================================================================
