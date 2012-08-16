#include "ClupatraProcessor.h"

#include <time.h>
#include <vector>
#include <map>
#include <set>
#include <algorithm>

#include <math.h>
#include <cmath>
#include <memory>

//---- MarlinUtil 
#include "MarlinCED.h"

//---- LCIO ---
#include "IMPL/LCCollectionVec.h"
#include "IMPL/TrackImpl.h"
#include "IMPL/TrackerHitImpl.h"
#include "EVENT/SimTrackerHit.h"
#include "IMPL/LCFlagImpl.h"
#include "UTIL/Operators.h"
#include "UTIL/LCTOOLS.h"
#include "UTIL/CellIDDecoder.h"
#include "UTIL/ILDConf.h"

#include "LCIterator.h"

//-------gsl -----
#include "gsl/gsl_randist.h"
#include "gsl/gsl_cdf.h"


//---- GEAR ----
#include "marlin/Global.h"
#include "gear/GEAR.h"
#include "gear/TPCParameters.h"
#include "gear/ZPlanarParameters.h"
#include "gear/ZPlanarLayerLayout.h"
#include "gear/PadRowLayout2D.h"
#include "gear/BField.h"


#include "MarlinTrk/Factory.h"
#include "MarlinTrk/IMarlinTrack.h"
#include "MarlinTrk/IMarlinTrkSystem.h"
#include "MarlinTrk/MarlinTrkUtils.h"


using namespace lcio ;
using namespace marlin ;


#include <float.h>

#include "clupatra_new.h"
using namespace clupatra_new ;



/** helper method to create a track collections and add it to the event */
inline LCCollectionVec* newTrkCol(const std::string& name, LCEvent * evt , bool isSubset=false){

  LCCollectionVec* col = new LCCollectionVec( LCIO::TRACK ) ;  

  LCFlagImpl hitFlag(0) ;
  hitFlag.setBit( LCIO::TRBIT_HITS ) ;
  col->setFlag( hitFlag.getFlag()  ) ;

  evt->addCollection( col , name ) ;

  col->setSubset( isSubset ) ;

  return col ;
}
//----------------------------------------------------------------
/** helper method to get the collection from the event */
inline LCCollection* getCollection(  const std::string& name, LCEvent * evt ){

  LCCollection* col = 0 ;
  try{   col = evt->getCollection( name )  ; 
  } catch( lcio::DataNotAvailableException& e) { 
    streamlog_out( DEBUG4 ) <<  " input collection not in event : " << name << "  !!!  " << std::endl ;  
  } 
  return col ;
}

//----------------------------------------------------------------

template <class In, class Pred> In find_smallest(In first, In last, Pred p, double& d){
  
  In res =  last ;

  double min = DBL_MAX ;
 
  while( first != last ){

    double val = p( *first) ;

    if(  val < min ){

      res = first ;
      min = val ;
    }
 
    ++first ;
  }

  d = min ;

  streamlog_out( DEBUG ) << " found smallest " << *res << " min : " << d << std::endl ;

  return res ;
}
//----------------------------------------------------------------
struct Distance3D2{
  gear::Vector3D _pos ;
  Distance3D2( const gear::Vector3D& pos) : _pos( pos ) {}
  template <class T>
  double operator()( const T* t) { 
    gear::Vector3D p( t->getPosition() ) ;
    return ( p - _pos ).r2() ; 

  }
};

//----------------------------------------------------------------
struct StripDistance2{
  gear::Vector3D _pos ;
  StripDistance2( const gear::Vector3D& pos) : _pos( pos ) {}
  
  double operator()( const TrackerHit* t) { 

    gear::Vector3D p( t->getPosition() ) ;

    const TrackerHitPlane* h = (const TrackerHitPlane*) t ;

    gear::Vector3D v( 1. , h->getU()[1] ,  h->getU()[0] , gear::Vector3D::spherical ) ;

    double d = ( p - _pos ).dot(v)  ;

    streamlog_out( DEBUG ) << " h: " << *h << "\n"
			   << " v: " << v << "\n"
			   << " d: " << d << std::endl ;
    

    return d*d ;
  }
};

//----------------------------------------------------------------
struct MeanAbsZOfTrack{
  double operator()( const Track* t){
    double z = 0 ;
    int hitCount = 0 ;
    const TrackerHitVec& hV = t->getTrackerHits() ;
    for(unsigned i=0, N = hV.size() ; i<N ; ++i){
      z += hV[i]->getPosition()[2]  ;
      ++hitCount ;
    }
    return std::abs(  z )  / hitCount ;
  }
};

//----------------------------------------------------------------
void printTrackerHit(const EVENT::LCObject* o){
  
  lcio::TrackerHit* hit = const_cast<lcio::TrackerHit*> ( dynamic_cast<const lcio::TrackerHit*> (o) ) ;
  
  if( hit == 0 ) {
    
    streamlog_out( ERROR ) << " printTrackerHit : dynamic_cast<TrackerHit*> failed for LCObject : " << o << std::endl ;
    return ;
    
  } else {
    
    streamlog_out( MESSAGE )  << " --- TrackerHit: " << *hit  
			      << "\n --- delta Chi2 = " << hit->ext<DChi2>() << std::endl ;
  }
}
//----------------------------------------------------------------
void printSimTrackerHit(const EVENT::LCObject* o){
  
  lcio::SimTrackerHit* hit = const_cast<lcio::SimTrackerHit*> ( dynamic_cast<const lcio::SimTrackerHit*> (o) ) ;
  
  if( hit == 0 ) {
    
    streamlog_out( ERROR ) << " printSimTrackerHit : dynamic_cast<SimTrackerHit*> failed for LCObject : " << o << std::endl ;
    return ;
    
  } else {
    
    streamlog_out( MESSAGE )  << " --- SimTrackerHit: " << *hit  << "\n"
			      << " MCParticle: " <<  lcshort( hit->getMCParticle() ) 
			      << std::endl ;
  }
}
//----------------------------------------------------------------




ClupatraProcessor aClupatraProcessor ;


ClupatraProcessor::ClupatraProcessor() : Processor("ClupatraProcessor") {
  
  // modify processor description
  _description = "ClupatraProcessor : nearest neighbour clustering seeded pattern recognition" ;
  
  
  
  registerInputCollection( LCIO::TRACKERHIT,
			   "TPCHitCollection" , 
			   "Name of the tpc hit input collections"  ,
			   _colName ,
			   "AllTPCTrackerHits"  ) ;
  
  
  registerOutputCollection( LCIO::TRACK,
			    "OutputCollection" , 
			    "Name of the output collection with final TPC tracks"  ,
			    _outColName ,
			    std::string("ClupatraTracks" ) ) ;
  
  registerOutputCollection( LCIO::TRACK,
			    "SegmentCollectionName" , 
			    "Name of the output collection that has the individual track segments"  ,
			    _segmentsOutColName ,
			    std::string("ClupatraTrackSegments" ) ) ;
  
  registerProcessorParameter( "DistanceCut" , 
			      "Cut for distance between hits in mm for the seed finding"  ,
			      _distCut ,
			      (float) 40.0 ) ;


  registerProcessorParameter( "CosAlphaCut" , 
			      "Cut for max.angle between hits in consecutive layers for seed finding - NB value should be smaller than 1 - default is 0.9999999 !!!"  ,
			      _cosAlphaCut ,
			      (float) 0.9999999 ) ;

  registerProcessorParameter( "NLoopForSeeding" , 
 			      "number of seed finding loops - every loop increases the distance cut by DistanceCut/NLoopForSeeding"  ,
 			      _nLoop ,
 			      (int) 4 ) ;
  
  
  registerProcessorParameter( "MinimumClusterSize" , 
			      "minimum number of hits per cluster"  ,
			      _minCluSize ,
			      (int) 6) ;
  
  
  registerProcessorParameter( "DuplicatePadRowFraction" , 
			      "allowed fraction of hits in same pad row per track"  ,
			      _duplicatePadRowFraction,
			      (float) 0.1 ) ;
  
  // registerProcessorParameter( "RCut" , 
  // 			      "Cut for r_min in mm"  ,
  // 			      _rCut ,
  // 			      (float) 0.0 ) ;
  
  registerProcessorParameter( "MaxDeltaChi2" , 
 			      "the maximum delta Chi2 for which a hit is added to a track segement"  ,
 			      _dChi2Max ,
 			      (float) 35. ) ;

  registerProcessorParameter( "Chi2Cut" , 
 			      "the maximum chi2-distance for which a hit is considered for merging "  ,
 			      _chi2Cut ,
 			      (float) 100. ) ;
  
  registerProcessorParameter( "MaxStepWithoutHit" , 
 			      "the maximum number of layers without finding a hit before hit search search is stopped "  ,
 			      _maxStep ,
 			      (int) 3 ) ;


  registerProcessorParameter( "PadRowRange" , 
			      "number of pad rows used in initial seed clustering"  ,
			      _padRowRange ,
			      (int) 12) ;
 
  registerProcessorParameter( "NumberOfZBins" , 
			      "number of bins in z over total length of TPC - hits from different z bins are nver merged"  ,
			      _nZBins,
			      (int) 150 ) ;


  registerProcessorParameter( "MinLayerFractionWithMultiplicity" , 
			      "minimum fraction of layers that have a given multiplicity, when forcing a cluster into sub clusters"  ,
			      _minLayerFractionWithMultiplicity,
			      (float) 0.5 ) ;

  registerProcessorParameter( "MinLayerNumberWithMultiplicity" , 
			      "minimum number of layers that have a given multiplicity, when forcing a cluster into sub clusters"  ,
			      _minLayerNumberWithMultiplicity,
			      (int) 3 ) ;

  registerProcessorParameter( "TrackStartsInnerDist" , 
			      "maximum radial distance [mm] from inner field cage of first hit, such that the track is considered to start at the beginning " ,
 			      _trackStartsInnerDist ,
 			      (float) 25. ) ;
  
  registerProcessorParameter( "TrackEndsOuterCentralDist" , 
			      "maximum radial distance [mm] from outer field cage of last hit, such that the track is considered to end at the end " ,
 			      _trackEndsOuterCentralDist ,
 			      (float) 25. ) ;
  
  registerProcessorParameter( "TrackEndsOuterForwardDist" , 
			      "maximum distance in z [mm] from endplate of last hit, such that the track is considered to end at the end " ,
 			      _trackEndsOuterForwardDist ,
 			      (float) 40. ) ;
  
  registerProcessorParameter( "TrackIsCurlerOmega" , 
			      "minimum curvature omega of a track segment for being considered a curler" ,
 			      _trackIsCurlerOmega ,
 			      (float) 0.001 ) ;

  registerProcessorParameter("MultipleScatteringOn",
			     "Use MultipleScattering in Fit",
			     _MSOn,
			     bool(true));
  
  registerProcessorParameter("EnergyLossOn",
			     "Use Energy Loss in Fit",
			     _ElossOn,
			     bool(true));
  
  registerProcessorParameter("SmoothOn",
			     "Smooth All Mesurement Sites in Fit",
			     _SmoothOn,
			     bool(false));

  registerProcessorParameter("pickUpSiHits",
			     "try to pick up hits from Si-trackers",
			     _pickUpSiHits,
			     bool(false));
  

  registerOptionalParameter( "SITHitCollection" , 
			     "name of the SIT hit collections - used to extend TPC tracks if (pickUpSiHits==true)"  ,
			     _sitColName ,
			     std::string("SITTrackerHits")  ) ;

  registerOptionalParameter( "VXDHitCollection" , 
			     "name of the VXD hit collections - used to extend TPC tracks if (pickUpSiHits==true)"  ,
			     _vxdColName ,
			     std::string("VTXTrackerHits")  ) ;
  
  registerProcessorParameter("CreateDebugCollections",
			     "optionally create some debug collection with intermediate track segments and used and unused hits",
			     _createDebugCollections,
			     bool(false));
}


void ClupatraProcessor::init() { 

  // usually a good idea to
  printParameters() ;
  
  _trksystem =  MarlinTrk::Factory::createMarlinTrkSystem( "KalTest" , marlin::Global::GEAR , "" ) ;
  
  _trksystem->setOption( MarlinTrk::IMarlinTrkSystem::CFG::useQMS,        _MSOn ) ;
  _trksystem->setOption( MarlinTrk::IMarlinTrkSystem::CFG::usedEdx,       _ElossOn) ;
  _trksystem->setOption( MarlinTrk::IMarlinTrkSystem::CFG::useSmoothing,  _SmoothOn) ;
  _trksystem->init() ;  
  
  _nRun = 0 ;
  _nEvt = 0 ;
  

  CEDPickingHandler::getInstance().registerFunction( LCIO::TRACKERHIT  , &printTrackerHit ) ; 

  // CEDPickingHandler::getInstance().registerFunction( LCIO::TRACK , &printTrackShort ) ; 
  CEDPickingHandler::getInstance().registerFunction( LCIO::SIMTRACKERHIT , &printSimTrackerHit ) ; 
  
}

void ClupatraProcessor::processRunHeader( LCRunHeader* run) { 

  _nRun++ ;
} 


void ClupatraProcessor::processEvent( LCEvent * evt ) { 
  
  //  clock_t start =  clock() ; 
  Timer timer ;
  unsigned t_init       = timer.registerTimer(" initialization      " ) ;
  unsigned t_seedtracks = timer.registerTimer(" extend seed tracks  " ) ;
  unsigned t_recluster  = timer.registerTimer(" recluster leftovers " ) ;
  unsigned t_split      = timer.registerTimer(" split clusters      " ) ;
  unsigned t_finalfit   = timer.registerTimer(" final refit         " ) ;
  unsigned t_merge      = timer.registerTimer(" merge segments      " ) ;
  unsigned t_pickup     = timer.registerTimer(" pick up Si hits     " ) ;
  
  timer.start() ;
  
  // the clupa wrapper hits that hold pointers to LCIO hits plus some additional parameters
  // create them in a vector for convenient memeory mgmt 
  std::vector<ClupaHit> clupaHits ;
  
  // on top of the clupahits we need the tiny wrappers for clustering - they are created on the heap
  // and we put them in a vector of pointers that takes ownership (i.e. deletes them at the end)
  HitVec nncluHits ;        
  nncluHits.setOwner( true ) ;


  // this is the final list of cluster tracks
  Clusterer::cluster_list cluList ;    
  cluList.setOwner() ;
  

  LCIOTrackConverter converter ;
  
  _gearTPC = &Global::GEAR->getTPCParameters() ;
  _padLayout = &_gearTPC->getPadLayout() ;

  _bfield = Global::GEAR->getBField().at( gear::Vector3D(0.,0.0,0.) ).z() ;

  unsigned maxTPCLayers =  _padLayout->getNRows() ;
  
  double driftLength = _gearTPC->getMaxDriftLength() ;
  ZIndex zIndex( -driftLength , driftLength , _nZBins  ) ; 
  

  LCCollection* col = 0 ;

  try{   col =  dynamic_cast<LCCollection*> (evt->getCollection( _colName )  ); 
    
  } catch( lcio::DataNotAvailableException& e) { 
    
    streamlog_out( WARNING ) <<  " input collection not in event : " << _colName << "   - nothing to do  !!! " << std::endl ;  
    
    return ;
  } 
      
  //===============================================================================================
  //   create clupa and clustering hits for every lcio hit
  //===============================================================================================
  
  CellIDDecoder<TrackerHit> idDec( col ) ;
  
  int nHit = col->getNumberOfElements() ;
  
  clupaHits.resize( nHit ) ;       // creates clupa hits (w/ default c'tor)
  nncluHits.reserve( nHit ) ;


  for(int i=0 ; i < nHit ; ++i ) {
    
    //------
    
    TrackerHit* th = (TrackerHit*) col->getElementAt(i) ;
    
    ClupaHit* ch  = & clupaHits[i] ; 
    
    Hit* gh =  new Hit( ch ) ;
    
    nncluHits.push_back( gh ) ;
    
    //-------
    th->ext<GHit>() = gh ;  // assign the clupa hit to the LCIO hit for memory mgmt
    
    ch->lcioHit = th ; 
    
    ch->pos = gear::Vector3D(  th->getPosition() ) ;
    
    //  int padIndex = padLayout.getNearestPad( ch->pos.rho() , ch->pos.phi() ) ;
    //    ch->layer = padLayout.getRowNumber( padIndex ) ;
    ch->layer = idDec( th )[ ILDCellID0::layer ] ;

    ch->zIndex = zIndex( th ) ;
    
    //ch->phiIndex = ....
    
  } 

  //--------------------------------------------------------------------------------------------------------- 
  
  std::sort( nncluHits.begin(), nncluHits.end() , ZSort() ) ;
  
  //--------------------------------------------------------------------------------------------------------- 
  
  HitListVector hitsInLayer( maxTPCLayers ) ;
  addToHitListVector(  nncluHits.begin(), nncluHits.end() , hitsInLayer  ) ;
  
  //---------------------------------------------------------------------------------------------------------

  //===============================================================================================
  //   create output collections  ( some optional )
  //===============================================================================================

  const bool writeSeedCluster        = _createDebugCollections ;
  const bool writeCluTrackSegments   = _createDebugCollections ;
  const bool writeLeftoverClusters   = _createDebugCollections ;
  const bool writeQualityTracks      = _createDebugCollections ;
  
  static const bool copyTrackSegments = false ;
  
  LCCollectionVec* seedCol =  ( writeSeedCluster        ?  newTrkCol( "ClupatraSeedCluster"          , evt )  :   0   )  ; 
  LCCollectionVec* cluCol  =  ( writeCluTrackSegments   ?  newTrkCol( "ClupatraInitialTrackSegments" , evt )  :   0   )  ; 
  LCCollectionVec* locCol  =  ( writeCluTrackSegments   ?  newTrkCol( "ClupatraLeftoverClusters"     , evt )  :   0   )  ; 

  LCCollectionVec* incSegCol  = ( writeCluTrackSegments   ?  newTrkCol( "ClupatraIncompleteSegments"   , evt , true )  :   0   )  ; 
  LCCollectionVec* curSegCol  = ( writeCluTrackSegments   ?  newTrkCol( "ClupatraCurlerSegments"       , evt , true )  :   0   )  ; 
  LCCollectionVec* finSegCol  = ( writeCluTrackSegments   ?  newTrkCol( "ClupatraFinalTrackSegments"   , evt , true )  :   0   )  ; 

  //LCCollectionVec* goodCol  = ( writeQualityTracks ?  newTrkCol( "ClupatraGoodQualityTracks" , evt ,true )  :   0   )  ; 
  //LCCollectionVec* fairCol  = ( writeQualityTracks ?  newTrkCol( "ClupatraFairQualityTracks" , evt ,true )  :   0   )  ; 
  LCCollectionVec* poorCol  = ( writeQualityTracks ?  newTrkCol( "ClupatraPoorQualityTracks" , evt ,true )  :   0   )  ; 

  LCCollectionVec* outerCol  = ( _createDebugCollections ?  newTrkCol( "ClupatraOuterSegments" , evt ,true )  :   0   )  ; 
  LCCollectionVec* innerCol  = ( _createDebugCollections ?  newTrkCol( "ClupatraInnerSegments" , evt ,true )  :   0   )  ; 
  LCCollectionVec* middleCol = ( _createDebugCollections ?  newTrkCol( "ClupatraMiddleSegments" , evt ,true )  :   0   )  ; 

  
  LCCollectionVec* tsCol  =  newTrkCol( _segmentsOutColName , evt ) ;
  
  LCCollectionVec* outCol =  newTrkCol( _outColName  , evt )  ; 

  //---------------------------------------------------------------------------------------------------------
  
  timer.time(t_init ) ; 

  //===============================================================================================
  // first main step of clupatra:
  //   * cluster in pad row range - starting from the outside - to find clean cluster segments
  //   * extend the track segments with best matching hits, based on extrapolation to next layer(s)
  //   * add the hits and apply a Kalman filter step ( track segement is always best estimate )
  //   * repeat in backward direction ( after smoothing back, to get a reasonable track 
  //     state for extrapolating backwards )
  //===============================================================================================

  Clusterer nncl ;
  
  int outerRow = 0 ;
  
  nnclu::PtrVector<MarlinTrk::IMarlinTrack> seedTrks ;
  seedTrks.setOwner() ; // memory mgmt - will delete MarlinTrks at the end
  
  IMarlinTrkFitter fitter( _trksystem ) ;


  // ---- introduce a loop over increasing distance cuts for finding the tracks seeds
  //      -> should fix (some of) the problems seen @ 3 TeV with extremely boosted jets
  //
  int NLoop = _nLoop ; // fixme make parameter
  double dcut =  _distCut / NLoop ;
  for(int nloop=1 ; nloop <= NLoop ; ++nloop){ 

    HitDistance dist( nloop * dcut , _cosAlphaCut ) ;

    outerRow = maxTPCLayers - 1 ;
    
    
    while( outerRow >= _minCluSize ) { //_padRowRange * .5 ) {
      
      HitVec hits ;
      hits.reserve( nHit ) ;
      
      // add all hits in pad row range to hits
      for(int iRow = outerRow ; iRow > ( outerRow - _padRowRange) ; --iRow ) {
	if( iRow > -1 ) 
	  std::copy( hitsInLayer[ iRow ].begin() , hitsInLayer[ iRow ].end() , std::back_inserter( hits )  ) ;
      }
      
      //-----  cluster in given pad row range  -----------------------------
      Clusterer::cluster_list sclu ;    
      sclu.setOwner() ;  
    
      nncl.cluster_sorted( hits.begin(), hits.end() , std::back_inserter( sclu ), dist , _minCluSize ) ;
    
      const static int merge_seeds = true ; 

      if( merge_seeds ) { //-----------------------------------------------------------------------
	
	// sometimes we have split seed clusters as one link is just above the cut
	// -> recluster in all hits of small clusters with 1.2 * cut 
	float _smallClusterPadRowFraction = 0.9  ;
	float _cutIncrease = 1.2 ;
	// fixme: could make parameters ....

	HitVec seedhits ;
	Clusterer::cluster_list smallclu ; 
	smallclu.setOwner() ;      
	split_list( sclu, std::back_inserter(smallclu),  ClusterSize(  int( _padRowRange * _smallClusterPadRowFraction) ) ) ; 
	for( Clusterer::cluster_list::iterator sci=smallclu.begin(), end= smallclu.end() ; sci!=end; ++sci ){
	  for( Clusterer::cluster_type::iterator ci=(*sci)->begin(), end= (*sci)->end() ; ci!=end;++ci ){
	    seedhits.push_back( *ci ) ; 
	  }
	}
	// free hits from bad clusters 
	std::for_each( smallclu.begin(), smallclu.end(), std::mem_fun( &CluTrack::freeElements ) ) ;
	
	HitDistance distLarge( nloop * dcut * _cutIncrease ) ;

	nncl.cluster_sorted( seedhits.begin(), seedhits.end() , std::back_inserter( sclu ), distLarge , _minCluSize ) ;

      } //------------------------------------------------------------------------------------------

      // try to split up clusters according to multiplicity
      int layerWithMultiplicity = _padRowRange - 2  ; // fixme: make parameter 
      split_multiplicity( sclu , layerWithMultiplicity , 10 ) ;


      // remove clusters whith too many duplicate hits per pad row
      Clusterer::cluster_list bclu ;    // bad clusters  
      bclu.setOwner() ;      
      split_list( sclu, std::back_inserter(bclu),  DuplicatePadRows( maxTPCLayers, _duplicatePadRowFraction  ) ) ;
      // free hits from bad clusters 
      std::for_each( bclu.begin(), bclu.end(), std::mem_fun( &CluTrack::freeElements ) ) ;

     
      // ---- now we also need to remove the hits from good cluster seeds from the hitsInLayers:
      for( Clusterer::cluster_list::iterator sci=sclu.begin(), end= sclu.end() ; sci!=end; ++sci ){
	for( Clusterer::cluster_type::iterator ci=(*sci)->begin(), end= (*sci)->end() ; ci!=end;++ci ){
	
	  // this is not cheap ...
	  hitsInLayer[ (*ci)->first->layer ].remove( *ci )  ; 
	}
      }
    
      // now we have 'clean' seed clusters
      if( writeSeedCluster ) {
	std::transform( sclu.begin(), sclu.end(), std::back_inserter( *seedCol ) , converter ) ;
      }
      
      //      std::transform( sclu.begin(), sclu.end(), std::back_inserter( seedTrks) , fitter ) ;
      // reduce memory footprint: deal with one KalTest track at a time and delete it, when done
    
      for( Clusterer::cluster_list::iterator icv = sclu.begin(), end =sclu.end()  ; icv != end ; ++ icv ) {
      
	int nHitsAdded = 0 ;

	MarlinTrk::IMarlinTrack* mTrk = fitter( *icv ) ;

	nHitsAdded += addHitsAndFilter( *icv , hitsInLayer , _dChi2Max, _chi2Cut , _maxStep , zIndex ) ; 
      
	static const bool backward = true ;
	nHitsAdded += addHitsAndFilter( *icv , hitsInLayer , _dChi2Max, _chi2Cut , _maxStep , zIndex, backward ) ; 

	// cannot drop seed clusters with no hits added as we would loos in the very forward region...
	// if( nHitsAdded < 1 ){ // poor seed segment -> delete cluster and free hits up again
	//   for( Clusterer::cluster_type::iterator ci=(*icv)->begin(), end= (*icv)->end() ; ci!=end; ++ci ) {
	//     hitsInLayer[ (*ci)->first->layer ].push_back( *ci )   ; 
	//   }
	//   (*icv)->freeElements() ;
	//   (*icv)->clear() ;
	// }
	
	if( writeCluTrackSegments )  //  ---- store track segments from the first main step  ----- 
	  cluCol->addElement(  converter( *icv ) );
	
	// reset the pointer to the KalTest track - as we are done with this track
	(*icv)->ext<MarTrk>() = 0 ;
	
	delete mTrk ;
      } 

      // merge the good clusters to final list
      cluList.merge( sclu ) ;

      outerRow -= _padRowRange ;
    
    } //while outerRow > padRowRange 
  
  }// nloop

  //---------------------------------------------------------------------------------------------------------

  timer.time( t_seedtracks ) ;
  
  timer.time( t_recluster ) ;
  
  //===============================================================================================
  //  do a global reclustering in leftover hits
  //===============================================================================================
  static const int do_global_reclustering = true ;
  if( do_global_reclustering ) {

    outerRow = maxTPCLayers - 1 ;
    
    int padRangeRecluster = 50 ; // FIXME: make parameter 
    // define an inner cylinder where we exclude hits from re-clustering:
    double zMaxInnerHits   = driftLength * .67 ;   // FIXME: make parameter 
    double rhoMaxInnerHits = _padLayout->getPlaneExtent()[0] + (  _padLayout->getPlaneExtent()[1] - _padLayout->getPlaneExtent()[0] ) * .67 ;// FIXME: make parameter 
    
    streamlog_out( DEBUG5 ) << "  ===========================================================================\n"
			    << "      recluster in leftover hits - outside a clyinder of :  z =" << zMaxInnerHits << " rho = " <<  rhoMaxInnerHits << "\n"
			    << "  ===========================================================================\n" << std::endl ;
    
    
    while( outerRow > 0 ) {
      
      
      Clusterer::cluster_list loclu ; // leftover clusters
      loclu.setOwner() ;
      
      HitVec hits ;
      
      int  minRow = ( ( outerRow - padRangeRecluster ) > -1 ?  ( outerRow - padRangeRecluster ) : -1 ) ;
      
      // add all hits in pad row range to hits
      for(int iRow = outerRow ; iRow > minRow ; --iRow ) {
	
	streamlog_out( DEBUG ) << "      hit candidates in row " << iRow << " : " << hitsInLayer[ iRow ].size() << std::endl ;
	
	for( HitList::iterator hlIt=hitsInLayer[ iRow ].begin() , end = hitsInLayer[ iRow ].end() ; hlIt != end ; ++hlIt ) {
	  streamlog_out( DEBUG ) << "      hit candidate for reclustering " << (*hlIt)->first 
				 << " ( std::abs( (*hlIt)->first->pos.z() ) > zMaxInnerHits  ||  (*hlIt)->first->pos.rho() >  rhoMaxInnerHits )  " 
				 <<   ( std::abs( (*hlIt)->first->pos.z() ) > zMaxInnerHits  ||  (*hlIt)->first->pos.rho() >  rhoMaxInnerHits )
				 << std::endl ;
	  
	  if( std::abs( (*hlIt)->first->pos.z() ) > zMaxInnerHits  ||  (*hlIt)->first->pos.rho() >  rhoMaxInnerHits ) {
	    hits.push_back( *hlIt ) ;
	  }
	}
      }
      
      
      HitDistance distSmall( _distCut ) ; 
      nncl.cluster( hits.begin(), hits.end() , std::back_inserter( loclu ),  distSmall , _minCluSize ) ;
      
      streamlog_out( DEBUG ) << "   reclusterd in the range : " << outerRow << " - " <<  minRow 
			     << " found " << loclu.size() << " clusters " 
			     << std::endl ;
      
      if( writeLeftoverClusters )
	std::transform( loclu.begin(), loclu.end(), std::back_inserter( *locCol ) , converter ) ;
      
      
      // timer.time( t_recluster ) ;
      
      //===============================================================================================
      //  now we split the clusters based on their hit multiplicities
      //===============================================================================================
      
      
      //    _dChi2Max = 5. * _dChi2Max ; //FIXME !!!!!!!!!
      
      for( Clusterer::cluster_list::iterator it= loclu.begin(), end= loclu.end() ; it != end ; ++it ){
	
	CluTrack* clu = *it ;
	
	streamlog_out(  DEBUG5 ) << " **** left over cluster with size : " << clu->size() << std::endl ;
	
	std::vector<int> mult(8) ; 
	// get hit multiplicities up to 6 ( 7 means 7 or higher ) 
	getHitMultiplicities( clu , mult ) ;
	
	streamlog_out(  DEBUG3 ) << " **** left over cluster with hit multiplicities: \n" ;
	for( unsigned i=0,n=mult.size() ; i<n ; ++i) {
	  streamlog_out(  DEBUG3 ) << "     m["<<i<<"] = " <<  mult[i] << "\n"  ;
	}
	
	
	if( float( mult[5]) / mult[0]  >= _minLayerFractionWithMultiplicity &&  mult[5] >  _minLayerNumberWithMultiplicity ) {
	  
	  Clusterer::cluster_list reclu ; // reclustered leftover clusters
	  reclu.setOwner() ;
	  
	  create_n_clusters( *clu , reclu , 5 ) ;
	  
	  std::transform( reclu.begin(), reclu.end(), std::back_inserter( seedTrks) , fitter ) ;
	  
	  for( Clusterer::cluster_list::iterator ir= reclu.begin(), end= reclu.end() ; ir != end ; ++ir ){
	    
	    streamlog_out( DEBUG5 ) << " extending mult-5 clustre  of length " << (*ir)->size() << std::endl ;
	    
	    addHitsAndFilter( *ir , hitsInLayer , _dChi2Max, _chi2Cut , _maxStep , zIndex) ; 
	    static const bool backward = true ;
	    addHitsAndFilter( *ir , hitsInLayer , _dChi2Max, _chi2Cut , _maxStep , zIndex, backward ) ; 
	  }
	  
	  cluList.merge( reclu ) ;
	} 
      
	else if( float( mult[4]) / mult[0]  >= _minLayerFractionWithMultiplicity &&  mult[4] >  _minLayerNumberWithMultiplicity ) {
	
	  Clusterer::cluster_list reclu ; // reclustered leftover clusters
	  reclu.setOwner() ;
	
	  create_n_clusters( *clu , reclu , 4 ) ;
	
	  std::transform( reclu.begin(), reclu.end(), std::back_inserter( seedTrks) , fitter ) ;
	
	  for( Clusterer::cluster_list::iterator ir= reclu.begin(), end= reclu.end() ; ir != end ; ++ir ){
	  
	    streamlog_out( DEBUG5 ) << " extending mult-4 clustre  of length " << (*ir)->size() << std::endl ;
	  
	    addHitsAndFilter( *ir , hitsInLayer , _dChi2Max, _chi2Cut , _maxStep , zIndex) ; 
	    static const bool backward = true ;
	    addHitsAndFilter( *ir , hitsInLayer , _dChi2Max, _chi2Cut , _maxStep , zIndex, backward ) ; 
	  }
	
	  cluList.merge( reclu ) ;
	} 
      
	else if( float( mult[3]) / mult[0]  >= _minLayerFractionWithMultiplicity &&  mult[3] >  _minLayerNumberWithMultiplicity ) {
	
	  Clusterer::cluster_list reclu ; // reclustered leftover clusters
	  reclu.setOwner() ;
	
	  create_three_clusters( *clu , reclu ) ;
	
	  std::transform( reclu.begin(), reclu.end(), std::back_inserter( seedTrks) , fitter ) ;
	
	  for( Clusterer::cluster_list::iterator ir= reclu.begin(), end= reclu.end() ; ir != end ; ++ir ){
	  
	    streamlog_out( DEBUG5 ) << " extending triplet clustre  of length " << (*ir)->size() << std::endl ;
	  
	    addHitsAndFilter( *ir , hitsInLayer , _dChi2Max, _chi2Cut , _maxStep , zIndex) ; 
	    static const bool backward = true ;
	    addHitsAndFilter( *ir , hitsInLayer , _dChi2Max, _chi2Cut , _maxStep , zIndex, backward ) ; 
	  }
	
	  cluList.merge( reclu ) ;
	} 
      
	else if( float( mult[2]) / mult[0]  >= _minLayerFractionWithMultiplicity &&  mult[2] >  _minLayerNumberWithMultiplicity ) {
	
	  Clusterer::cluster_list reclu ; // reclustered leftover clusters
	  reclu.setOwner() ;
	
	  create_two_clusters( *clu , reclu ) ;
	
	  std::transform( reclu.begin(), reclu.end(), std::back_inserter( seedTrks) , fitter ) ;
	
	  for( Clusterer::cluster_list::iterator ir= reclu.begin(), end= reclu.end() ; ir != end ; ++ir ){
	  
	    streamlog_out( DEBUG5 ) << " extending doublet clustre  of length " << (*ir)->size() << std::endl ;
	  
	    addHitsAndFilter( *ir , hitsInLayer , _dChi2Max, _chi2Cut , _maxStep , zIndex) ; 
	    static const bool backward = true ;
	    addHitsAndFilter( *ir , hitsInLayer , _dChi2Max, _chi2Cut , _maxStep , zIndex, backward ) ; 
	  } 
	
	  cluList.merge( reclu ) ;
	
	}
	else if( float( mult[1]) / mult[0]  >= _minLayerFractionWithMultiplicity &&  mult[1] >  _minLayerNumberWithMultiplicity ) {    
	
	
	  seedTrks.push_back( fitter( *it )  );
	
	  addHitsAndFilter( *it , hitsInLayer , _dChi2Max, _chi2Cut , _maxStep , zIndex) ; 
	  static const bool backward = true ;
	  addHitsAndFilter( *it , hitsInLayer , _dChi2Max, _chi2Cut , _maxStep , zIndex, backward ) ; 
	
	  cluList.push_back( *it ) ;
	
	  it = loclu.erase( it ) ;
	  --it ; // erase returns iterator to next element 
	
	} else {
	
	  //  discard cluster and free hits
	  clu->freeElements() ; 
	}
      
      }

  
      outerRow -=  padRangeRecluster ; 

    }
  }

  //=======================================================================================================================
  //  try again to gobble up hits at the ends ....   - does not work right now, as there are no fits  for the clusters....
  //=======================================================================================================================

  // streamlog_out( DEBUG5 ) << " ===========     gobble up leftover hits at the ends for "  <<  cluList.size() << "  clusters " << std::endl ;
  
  // for( Clusterer::cluster_list::iterator icv = cluList.begin() , end = cluList.end() ; icv != end ; ++ icv ) {
    
  //   if( (*icv)->empty() ) continue ;
    
  //   int nH = 0 ;

  //   nH += addHitsAndFilter( *icv , hitsInLayer , _dChi2Max, _chi2Cut , _maxStep , zIndex) ; 
  //   static const bool backward = true ;
  //   nH += addHitsAndFilter( *icv , hitsInLayer , _dChi2Max, _chi2Cut , _maxStep , zIndex, backward ) ; 

  //   streamlog_out( DEBUG3 ) << "     added " << nH << " leftover hits to cluster " << *icv << std::endl ; 
  // }
  
  timer.time( t_split ) ;

  //===============================================================================================
  //  now refit the tracks 
  //===============================================================================================

  streamlog_out( DEBUG5 ) << " ===========    refitting final " << cluList.size() << " track segments  "   << std::endl ;

  //---- refit cluster tracks individually to save memory ( KalTest tracks have ~1MByte each)

  IMarlinTrkFitter fit(_trksystem,  _dChi2Max) ; // fixme: do we need a different chi2 max here ????

  for( Clusterer::cluster_list::iterator icv = cluList.begin() , end = cluList.end() ; icv != end ; ++ icv ) {

    if( (*icv)->empty() ) 
      continue ;

    MarlinTrk::IMarlinTrack* trk = fit( *icv ) ;
    trk->smooth() ;
    Track* lcioTrk = converter( *icv ) ; 
    tsCol->push_back(  lcioTrk ) ;
    lcioTrk->ext<MarTrk>() = 0 ;
    delete trk ;
  }
  
  timer.time( t_finalfit) ;
  
  //===============================================================================================
  //   optionally create collections of used and unused TPC hits 
  //===============================================================================================
  
  if( _createDebugCollections ) {
    LCCollectionVec* usedHits   = new LCCollectionVec( LCIO::TRACKERHIT ) ;   ;
    LCCollectionVec* unUsedHits = new LCCollectionVec( LCIO::TRACKERHIT ) ;   ;
    evt->addCollection( usedHits ,   "ClupatraUsedTPCHits"   ) ;
    evt->addCollection( unUsedHits , "ClupatraUnUsedTPCHits" ) ;
    usedHits->setSubset() ;
    unUsedHits->setSubset() ;
    usedHits->reserve(   nncluHits.size() ) ;
    unUsedHits->reserve( nncluHits.size() ) ;
    
    for( HitVec::iterator it = nncluHits.begin(), end = nncluHits.end(); it!=end;++it ){
      
      if( (*it)->second != 0 ){   usedHits->push_back( (*it)->first->lcioHit ) ;
      } else {                  unUsedHits->push_back( (*it)->first->lcioHit ) ;          
      }
    }
  }



  //===============================================================================================
  //  compute some track parameters for possible merging
  //===============================================================================================
  
  typedef nnclu::NNClusterer<Track> TrackClusterer ;
  TrackClusterer nntrkclu ;
  MakeLCIOElement<Track> trkMakeElement ;
  
  for( int i=0,N=tsCol->getNumberOfElements() ;  i<N ; ++i ) {
    
    computeTrackInfo( (Track*) tsCol->getElementAt(i) ) ;
  }
  
  //===============================================================================================
  //  merge split segements 
  //===============================================================================================
  
  
  static const int merge_split_segments = true ;

  if( merge_split_segments ) {

    for(unsigned l=0 ; l < 2 ; ++l ) { // do this twice ....
      
      streamlog_out( DEBUG5 ) << "===============================================================================================\n"
			      << "  merge split segments\n"
			      << "===============================================================================================\n"  ;
      
      int nMax  =  tsCol->size()   ;
      
      TrackClusterer::element_vector incSegVec ;
      incSegVec.setOwner() ;
      incSegVec.reserve( nMax  ) ;
      TrackClusterer::cluster_vector incSegCluVec ;
      incSegCluVec.setOwner() ;
      
      for( int i=0,N=tsCol->getNumberOfElements() ;  i<N ; ++i ){
	
	TrackImpl* trk = (TrackImpl*) tsCol->getElementAt(i) ;
	
	const TrackInfoStruct* ti = trk->ext<TrackInfo>() ;
	
	bool isIncompleteSegment =   !ti->isCurler  && ( !ti->startsInner || ( !ti->isCentral && !ti->isForward )  ) ;  
	
	std::bitset<32> type = trk->getType() ;


	if( isIncompleteSegment  && ! type[ ILDTrackTypeBit::SEGMENT ]){ 
	  
	  incSegVec.push_back(  trkMakeElement( trk )  ) ; 
	  
	  if( writeCluTrackSegments )  incSegCol->addElement( trk ) ;
	}
      }
      
 
      TrackSegmentMerger trkMerge( _dChi2Max , _trksystem ,  _bfield  ) ; 
 
      nntrkclu.cluster( incSegVec.begin() , incSegVec.end() , std::back_inserter( incSegCluVec ), trkMerge , 2  ) ;

      streamlog_out( DEBUG4 ) << " ===== merged track segments - # cluster: " << incSegCluVec.size()   
			      << " from " << incSegVec.size() << " incomplete track segments "    << "  ============================== " << std::endl ;
    
      for(  TrackClusterer::cluster_vector::iterator it= incSegCluVec.begin() ; it != incSegCluVec.end() ; ++it) {
      
	streamlog_out( DEBUG4 ) <<  lcio::header<Track>() << std::endl ;
      
	TrackClusterer::cluster_type*  incSegClu = *it ;

	std::vector<Track*> mergedTrk ;
      
	// vector to collect hits from segments
	//      std::vector< TrackerHit* >  hits ;
	// hits.reserve( 1024 ) ;
	// IMPL::TrackImpl* track = new  IMPL::TrackImpl ;
	// tsCol->addElement( track ) ;
      
	CluTrack hits ; 
      
	for( TrackClusterer::cluster_type::iterator itC = incSegClu->begin() ; itC != incSegClu->end() ; ++ itC ){
	
	  streamlog_out( DEBUG3 ) << lcshort(  (*itC)->first ) << std::endl ; 
	
	  TrackImpl* trk = (TrackImpl*) (*itC)->first ;

	  mergedTrk.push_back( trk ) ;

	  //	std::copy( trk->getTrackerHits().begin() , trk->getTrackerHits().end() , std::back_inserter( hits ) ) ;

	  for( lcio::TrackerHitVec::const_iterator it = trk->getTrackerHits().begin() , END =  trk->getTrackerHits().end() ; it != END ; ++ it ){
	    hits.addElement( (*it)->ext<GHit>() )  ;
	  }

	  // flag the segments so they can be ignored for final list 
	  trk->setTypeBit( ILDTrackTypeBit::SEGMENT ) ;

	  // add old segments to new track
	  //	track->addTrack( trk ) ;
	}

	// MarlinTrk::IMarlinTrack* mTrk = _trksystem->createTrack();
	// EVENT::FloatVec icov( 15 ) ;
	// icov[ 0] = 1e2 ;
	// icov[ 2] = 1e2 ;
	// icov[ 5] = 1e2 ;
	// icov[ 9] = 1e2 ;
	// icov[14] = 1e2 ;
	// int result = createFinalisedLCIOTrack( mTrk, hits, track, !MarlinTrk::IMarlinTrack::backward, icov, _bfield,  _dChi2Max ) ;
	// //int result = createFinalisedLCIOTrack( mTrk, hits, track, ! MarlinTrk::IMarlinTrack::backward, icov, _bfield,  _dChi2Max ) ; 
	// // ??? 
      
	MarlinTrk::IMarlinTrack* mTrk = fit( &hits ) ;
	mTrk->smooth() ;
	Track* track = converter( &hits ) ; 
	tsCol->push_back(  track ) ;
	track->ext<MarTrk>() = 0 ;
	delete mTrk ;
	computeTrackInfo( track ) ;    

	streamlog_out( DEBUG4 ) << "   ******  created new track : " << " : " << lcshort( (Track*) track )  << std::endl ;

      }
    }// loop over l 
  }
  //===============================================================================================
  //  merge curler segments 
  //===============================================================================================
  
  
  static const int merge_curler_segments = true ;
  
  if( merge_curler_segments ) {


    streamlog_out( DEBUG5 ) << "===============================================================================================\n"
			    << "  merge curler segments\n"
			    << "===============================================================================================\n"  ;
    
    int nMax  =  tsCol->size()   ;

    TrackClusterer::element_vector curSegVec ;
    curSegVec.setOwner() ;
    curSegVec.reserve( nMax  ) ;
    TrackClusterer::cluster_vector curSegCluVec ;
    curSegCluVec.setOwner() ;


    //    for( int i=0,N=tsCol->getNumberOfElements() ;  i<N ; ++i ){
    for( int i=tsCol->getNumberOfElements()-1 ;  i>=0 ; --i ){
      
      TrackImpl* trk = (TrackImpl*) tsCol->getElementAt(i) ;
      

      std::bitset<32> type = trk->getType() ;

      if( type[ ILDTrackTypeBit::SEGMENT ] ) 
	continue ;   // ignore previously merged track segments

      const TrackInfoStruct* ti = trk->ext<TrackInfo>() ;
      
      bool isCompleteTrack =   ti && !ti->isCurler  && ( ti->startsInner &&  (  ti->isCentral || ti->isForward ) );  
      
      if( !isCompleteTrack ){ 
	
	curSegVec.push_back(  trkMakeElement( trk )  ) ; 
	
	if( writeCluTrackSegments )  curSegCol->addElement( trk ) ;
	  
      } else {   // ... is not a curler ->  add a copy to the final tracks collection 
	  

	if( copyTrackSegments) {

	  outCol->addElement( new TrackImpl( *trk )  ) ;

	}else{

	  outCol->addElement( trk ) ;

	  tsCol->removeElementAt( i ) ;
	}

	if( writeCluTrackSegments )  finSegCol->addElement( trk ) ;
      }
    }
    
    //======================================================================================================


    TrackCircleDistance trkMerge( 0.1 ) ; 

    nntrkclu.cluster( curSegVec.begin() , curSegVec.end() , std::back_inserter( curSegCluVec ), trkMerge , 2  ) ;


    streamlog_out( DEBUG4 ) << " ===== merged tracks - # cluster: " << curSegCluVec.size()   
			    << " from " << tsCol->size() << " track segments "    << "  ============================== " << std::endl ;
    
    for(  TrackClusterer::cluster_vector::iterator it= curSegCluVec.begin() ; it != curSegCluVec.end() ; ++it) {
      
      streamlog_out( DEBUG4 ) <<  lcio::header<Track>() << std::endl ;
      
      TrackClusterer::cluster_type*  curSegClu = *it ;

      std::list<Track*> mergedTrk ;

      for( TrackClusterer::cluster_type::iterator itC = curSegClu->begin() ; itC != curSegClu->end() ; ++ itC ){
	
	streamlog_out( DEBUG4 ) << lcshort(  (*itC)->first ) << std::endl ; 
	
	mergedTrk.push_back( (*itC)->first ) ; 
      }
      

      mergedTrk.sort( TrackZSort() ) ;
      
      //================================================================================

 
      if( copyTrackSegments) {

	// ====== create a new LCIO track for the merged cluster ...
	TrackImpl* trk = new TrackImpl ;
	
	// == and copy all the hits 
	unsigned hitCount = 0 ;
	for( std::list<Track*>::iterator itML = mergedTrk.begin() ; itML != mergedTrk.end() ; ++ itML ){
	  
	  const TrackerHitVec& hV = (*itML)->getTrackerHits() ;
	  for(unsigned i=0, n=hV.size() ; i<n ; ++i){
	    
	    trk->addHit( hV[i] ) ;
	  }
	  hitCount  += hV.size()  ;
	  
	  // add a pointer to the original track segment 
	  trk->addTrack( *itML ) ;
	}
	
	// take track states from first and last track :
	Track* firstTrk = mergedTrk.front() ;
	Track* lastTrk  = mergedTrk.back() ;
	
	const TrackState* ts = 0 ; 
	ts = firstTrk->getTrackState( lcio::TrackState::AtIP  ) ;
	if( ts ) trk->addTrackState( new TrackStateImpl( ts )  ) ;
	
	ts = firstTrk->getTrackState( lcio::TrackState::AtFirstHit  ) ;
	if( ts ) 	trk->addTrackState( new TrackStateImpl( ts )  ) ;
	
	ts = lastTrk->getTrackState( lcio::TrackState::AtLastHit  ) ;
	if( ts ) trk->addTrackState( new TrackStateImpl( ts )  ) ;
	
	ts = lastTrk->getTrackState( lcio::TrackState::AtCalorimeter  ) ;
	if( ts ) trk->addTrackState( new TrackStateImpl( ts )  ) ;
	
	
	trk->ext<MarTrk>() = firstTrk->ext<MarTrk>() ;
	
	int hitsInFit  =  firstTrk->getSubdetectorHitNumbers()[ 2 * ILDDetID::TPC - 1 ] ;
	trk->setChi2(     firstTrk->getChi2()     ) ;
	trk->setNdf(      firstTrk->getNdf()      ) ;
	trk->setdEdx(     firstTrk->getdEdx()     ) ;
	trk->setdEdxError(firstTrk->getdEdxError()) ;
	
	trk->subdetectorHitNumbers().resize( 2 * ILDDetID::ETD ) ;
	trk->subdetectorHitNumbers()[ 2 * ILDDetID::TPC - 2 ] =  hitsInFit ;  
	trk->subdetectorHitNumbers()[ 2 * ILDDetID::TPC - 1 ] =  hitCount ;  
	
	ts = trk->getTrackState( lcio::TrackState::AtFirstHit  ) ;
	double RMin = ( ts ?
			sqrt( ts->getReferencePoint()[0] * ts->getReferencePoint()[0]
			      + ts->getReferencePoint()[1] * ts->getReferencePoint()[1] )
			:  0.0 ) ;
	trk->setRadiusOfInnermostHit( RMin  ) ; 

    

	streamlog_out( DEBUG2 ) << "   create new merged track from bestTrack parameters - ptr to MarlinTrk : " << trk->ext<MarTrk>()  
				<< "   with subdetector hit numbers  : " <<  trk->subdetectorHitNumbers()[0 ] << " , " <<  trk->subdetectorHitNumbers()[1] 
				<< std::endl ;
	
	
	outCol->addElement( trk )  ;

      } else { //==========================
	
	// we move the first segment to the final list and keep pointers to the other segments

	std::list<Track*>::iterator itML = mergedTrk.begin() ;

	TrackImpl* trk = (TrackImpl*) *itML++ ;

	for(  ; itML != mergedTrk.end() ; ++itML ){
	  
	  // add a pointer to the original track segment 
	  trk->addTrack( *itML ) ;
	}

	outCol->addElement( trk ) ;

	//remove from segment collection:
	for( int i=0,N=tsCol->size() ; i<N ; ++i) {
	  if( ((Track*) tsCol->getElementAt(i) ) == trk ){ 
	    tsCol->removeElementAt( i ) ;
	    break ;
	  }
	}

      }//================================================================================

    }
    //---------------------------------------------------------------------------------------------
    // // add all tracks that have not been merged :
    
    for( TrackClusterer::element_vector::iterator it = curSegVec.begin(); it != curSegVec.end() ;++it){
      
      if( (*it)->second == 0 ){
	
    	TrackImpl* trk = dynamic_cast<TrackImpl*>( (*it)->first ) ;
	
	if( copyTrackSegments) {

	  TrackImpl* t =   new TrackImpl( *trk ) ;
	
	  t->ext<TrackInfo>() = 0 ; // set extension to 0 to prevent double free ... 
	
	  t->ext<MarTrk>() = 0 ; // dynamic_cast<TrackImpl*>( (*it)->first )->ext<MarTrk>() ;
	
	  streamlog_out( DEBUG2 ) << "   create new track from existing LCIO track  - ptr to MarlinTrk : " << t->ext<MarTrk>()  << std::endl ;
	
	  outCol->addElement( t ) ;

	} else { 
	  outCol->addElement( trk ) ;
	  
	  //remove from segment collection:
	  for( int i=0,N=tsCol->size() ; i<N ; ++i) {
	    if( ((Track*) tsCol->getElementAt(i) ) == trk ){ 
	      tsCol->removeElementAt( i ) ;
	      break ;
	    }
	  }
	}


      }
    }
    
  }
  timer.time( t_merge ) ;  



  //===============================================================================================
  //  create some debug collections ....
  //===============================================================================================
  if( _createDebugCollections ) {

    float r_inner = _padLayout->getPlaneExtent()[0] ;
    float r_outer = _padLayout->getPlaneExtent()[1] ;

    for(  LCIterator<TrackImpl> it( outCol ) ;  TrackImpl* trk = it.next()  ; ) {
      

      const TrackState* tsF = trk->getTrackState( lcio::TrackState::AtFirstHit  ) ;
      const TrackState* tsL = trk->getTrackState( lcio::TrackState::AtLastHit  ) ;
      
      gear::Vector3D fhPos( tsF->getReferencePoint() ) ;
      gear::Vector3D lhPos( tsL->getReferencePoint() ) ;
      

      bool startsInner =  std::abs( fhPos.rho() - r_inner )     <  _trackStartsInnerDist ;        // first hit close to inner field cage 
      bool isCentral   =  std::abs( lhPos.rho() - r_outer )     <  _trackEndsOuterCentralDist ;   // last hit close to outer field cage
      bool isForward   =  driftLength - std::abs( lhPos.z() )   <  _trackEndsOuterForwardDist  ;  // last hitclose to endcap
      bool isCurler    =  std::abs( tsF->getOmega() )           >  _trackIsCurlerOmega  ;         // curler segment ( r <~ 1m )
      bool endsOuter   = isCentral || isForward ;
     

      if( isCurler )  continue ;


      if( !startsInner && endsOuter ) {
	
	outerCol->addElement( trk ) ;
      } 
      if( startsInner &&  !endsOuter ) {
	
	innerCol->addElement( trk ) ;
      } 
      if( !startsInner &&  !endsOuter ) {    
	
	middleCol->addElement( trk ) ;
      }
      
    }  
  }
 //---------------------------------------------------------------------------------------------------------




  //---------------------------------------------------------------------------------------------------------
  //    pick up hits from Si trackers
  //---------------------------------------------------------------------------------------------------------

  if( _pickUpSiHits ){
    
    pickUpSiTrackerHits( outCol , evt ) ;
    
  }
  //---------------------------------------------------------------------------------------------------------

  timer.time( t_pickup ) ;  

  
  //---------------------------------------------------------------------------------------------------------
  //===============================================================================================
  //  apply some track quality cuts
  //===============================================================================================
  
  if( writeQualityTracks ) {
    for(  LCIterator<TrackImpl> it( outCol ) ;  TrackImpl* trk = it.next()  ; ) {
      
      // — Function: double gsl_cdf_chisq_P (double x, double nu)
      // — Function: double gsl_cdf_chisq_Q (double x, double nu)
      //cumulative distribution functions P(x) - lower , Q(x)  - upper 
      
      
      double prob = ( trk->getNdf() > 0 ? gsl_cdf_chisq_Q(  trk->getChi2() ,  (double) trk->getNdf() )  : 0. ) ;
      
      streamlog_out( DEBUG2 ) << " gsl_cdf_chisq_Q( "<< trk->getChi2() << ", " <<  (double) trk->getNdf()  << " ) = " << prob << std::endl ;
      if( prob < .01 ) // fixme - parameter ??
	poorCol->addElement( trk ) ;
      
      
      
      
    }
  }
 //---------------------------------------------------------------------------------------------------------




  streamlog_out( DEBUG9 )  <<  timer.toString () << std::endl ;

  _nEvt ++ ;



  // //DEBUG (check memory usage)
  // streamlog_out( MESSAGE5 )  << "\n hit return to continue " << std::endl ; 
  // char tmp ;
  // std::cin.getline(  &tmp,1 );


}


/*************************************************************************************************/
void ClupatraProcessor::pickUpSiTrackerHits( EVENT::LCCollection* trackCol , LCEvent* evt) {
  
  /*************************************************************************************************/
  
  streamlog_out( DEBUG3  ) << " ************ pickUpSiTrackerHits() called - nTracks : " << trackCol->getNumberOfElements() <<std::endl ;
  
  std::map< int , std::list<TrackerHit*> > hLMap ;
  
  UTIL::BitField64 encoder( ILDCellID0::encoder_string ) ; 
  

  if(  parameterSet( "SITHitCollection" ) ) {
    
    LCIterator<TrackerHit> it( evt, _sitColName ) ;
    while( TrackerHit* hit = it.next()  ){

      streamlog_out( DEBUG3  ) << "     adding SIT space point hit : " << hit << std::endl ;

      hLMap[ hit->getCellID0() ].push_back(  hit ) ;
    }    
  }
  if(  parameterSet( "VXDHitCollection" ) ) {
    
    LCIterator<TrackerHit> it( evt, _vxdColName ) ;
    while( TrackerHit* hit = it.next()  ){
      
      hLMap[ hit->getCellID0() ].push_back(  hit ) ;
    }    
  }


  streamlog_out( DEBUG3 ) << "  *****  hitMap size : " <<   hLMap.size() << std::endl ;
  
  for( std::map< int , std::list<TrackerHit*> >::iterator it= hLMap.begin(), End = hLMap.end() ; it != End ; ++it ){
    
    encoder.setValue( it->first ) ;
    
    streamlog_out( DEBUG3 ) << "  *****  sensor: " << encoder.valueString()  << " - nHits: " <<  it->second.size()  << std::endl ;
    
  }
  
  int nSITLayers = 0 ;
  try{
    const gear::ZPlanarParameters& gearSIT = Global::GEAR->getSITParameters() ;
    const gear::ZPlanarLayerLayout& gearSITLayout = gearSIT.getZPlanarLayerLayout() ;
    nSITLayers = gearSITLayout.getNLayers() ;
  }catch(gear::UnknownParameterException& ){}

  const gear::ZPlanarParameters& gearVXD = Global::GEAR->getVXDParameters() ;
  const gear::ZPlanarLayerLayout& gearVXDLayout = gearVXD.getZPlanarLayerLayout() ;
  int nVXDLayers = gearVXDLayout.getNLayers() ;

  int nLayers  = nVXDLayers + nSITLayers  ;


  // ============ sort tracks wrt pt (1./omega) ===============
  LCCollectionVec* tv  = dynamic_cast<LCCollectionVec*>(trackCol) ;

  std::sort( tv->begin() , tv->end() ,  PtSort()  ) ;
  
  for(  LCIterator<TrackImpl> it( trackCol ) ;  TrackImpl* trk = it.next()  ; ) 
    {
	

      double initial_chi2 = 0 ;
      int    initial_ndf  = 0 ;
      
      // ------------------------------------------
#if 1 // this code works for plain lcio tracks, i.e. in the case where the corresponding KalTrack
      // has already been deleted 
      //===========================================================================================

      //--------------------------------------------
      // create a temporary MarlinTrk
      //--------------------------------------------
      
      std::auto_ptr<MarlinTrk::IMarlinTrack> mTrk( _trksystem->createTrack()  ) ;

      //      double b = Global::GEAR->getBField().at( gear::Vector3D(0.,0.0,0.) ).z()  ;
      
      const EVENT::TrackState* ts = trk->getTrackState( lcio::TrackState::AtIP ) ; 
      
      //    const EVENT::TrackState* ts = trk->getClosestTrackState( 0., 0., 0. ) ;
      //    const IMPL::TrackStateImpl* ts = dynamic_cast<const IMPL::TrackStateImpl*>( trk->getClosestTrackState( 0., 0., 0. ) ) ;
      //    const IMPL::TrackStateImpl* ts = dynamic_cast<const IMPL::TrackStateImpl*>( trk->getTrackState( EVENT::TrackState::AtOther ) ) ;
      //  // FIXME:  what do we need here EVENT::TrackState::AtIP  or AtFirstHit ??
      
      int nHit = trk->getTrackerHits().size() ;
      
      if( nHit == 0 || ts ==0 )
	continue ;
      

      initial_chi2 = trk->getChi2() ;
      initial_ndf  = trk->getNdf() ;

      streamlog_out( DEBUG3  )  << "  -- extrapolate TrackState : " << lcshort( ts )    << std::endl ;
      
      //need to add a dummy hit to the track
      mTrk->addHit(  trk->getTrackerHits()[0] ) ; // fixme: make sure we got the right TPC hit here !??
      
      
      mTrk->initialise( *ts ,  _bfield ,  MarlinTrk::IMarlinTrack::backward ) ;
    
#else  //===========================================================================================
      // use the MarlinTrk allready stored with the TPC track


      MarlinTrk::IMarlinTrack* mTrk = trk->ext<MarTrk>()  ;
 
      //      const EVENT::TrackState* ts = trk->getTrackState( lcio::TrackState::AtIP ) ; 

      if( mTrk == 0 ){
	
	streamlog_out( DEBUG3  )  << "  ------ null pointer in ext<MarTrk> ! ?? ..... " << std::endl ;
	continue ;
      }
#endif //===========================================================================================

    //--------------------------------------------------
    // get intersection points with SIT and VXD layers 
    //-------------------------------------------------

    //    for( int l=nVXDLayers-1 ; l >=0 ; --l) {

    for( int lx=nLayers-1 ; lx >=0 ; --lx) {

      int detID = (  lx >= nVXDLayers  ?  ILDDetID::SIT   :  ILDDetID::VXD  ) ;
      int layer = (  lx >= nVXDLayers  ?  lx - nVXDLayers  :  lx              ) ;

      encoder.reset() ;
      encoder[ ILDCellID0::subdet ] = detID ;
      encoder[ ILDCellID0::layer  ] = layer ;
      int layerID = encoder.lowWord() ;  
      
      gear::Vector3D point ; 
      
      int sensorID = -1 ;

      int intersects = mTrk->intersectionWithLayer( layerID, point, sensorID, MarlinTrk::IMarlinTrack::modeClosest ) ;
      
      encoder.setValue( sensorID )  ;

      streamlog_out( DEBUG3 ) << " *******  pickUpSiTrackerHits - intersection with SIT/VXD layer " << layer 
			      << " intersects:  " << MarlinTrk::errorCode( intersects ) 
			      << " sensorID: " << encoder.valueString() 
			      << std::endl ;
      
      if( intersects == MarlinTrk::IMarlinTrack::success ){
	
	std::list<TrackerHit*>& hL = hLMap[ sensorID ] ;
	
	streamlog_out( DEBUG3 ) << "    **** found candidate hits : " << hL.size()  
				<< "         for point " << point << std::endl ;
	
	double min = 1.e99 ;
	double maxDist = 1. ; //FIXME: make parameter - what is reasonable here ?
	 
	std::list<TrackerHit*>::iterator bestIt ; 

	if( detID == ILDDetID::SIT ) {

	  bestIt = find_smallest( hL.begin(), hL.end() , StripDistance2( point ) , min ) ;

	} else {

	  bestIt = find_smallest( hL.begin(), hL.end() , Distance3D2( point ) , min ) ;
	}

	if( bestIt == hL.end() || min  > maxDist ){

	  streamlog_out( DEBUG3 ) << " ######### no close by hit found !! " 
				  << " (bestIt == hL.end())" << (bestIt == hL.end()) 
				  << " (min  > maxDist)" << (min  > maxDist) 
				  << std::endl ;
	  continue ; // FIXME: need to limit the number of layers w/o hits !!!!!!
	}

	double deltaChi ;

	streamlog_out( DEBUG3 ) << " will add best matching hit : " << *bestIt << " with distance : " << min << std::endl ;

	int addHit = mTrk->addAndFit( *bestIt , deltaChi, _dChi2Max ) ;
	    
	streamlog_out( DEBUG3 ) << "    ****  best matching hit : " <<  gear::Vector3D( (*bestIt)->getPosition() )  
				<< "         added : " << MarlinTrk::errorCode( addHit )
				<< "   deltaChi2: " << deltaChi 
				<< std::endl ;

	if( addHit ==  MarlinTrk::IMarlinTrack::success ){


	  trk->addHit( *bestIt ) ;
	  hL.erase( bestIt ) ;

	  IMPL::TrackStateImpl tsi ;
	  double chi2N; int ndfN ;

	  mTrk->getTrackState( tsi , chi2N , ndfN ) ; 

	  streamlog_out( DEBUG3  )  << "  -- extrapolate TrackState : " << lcshort( (TrackState*)&tsi )  << "\n" 
				    << " chi2: " << chi2N
				    << " ndfN: " << ndfN    
				    << std::endl ;

	}

      }
    }

    // -------------------------   update the track state ----------------------
    // FIXME: should this be done in processEvent()  ?
    lcio::TrackStateImpl* tsi =  new lcio::TrackStateImpl ;
    double chi2 ;
    int ndf  ;
    const gear::Vector3D ipv( 0.,0.,0. );
    
    
    // get track state at the IP 
    int ret = mTrk->propagate(   ipv, *tsi, chi2, ndf ) ;
    //    int ret = mTrk->extrapolate( ipv, *tsi, chi2, ndf ) ;
    
    if( ret == MarlinTrk::IMarlinTrack::success ){
      
      tsi->setLocation(  lcio::TrackState::AtIP ) ;
      

      // the track state at the IP needs to be the first one
      //  -> we have to copy the whole vector, and then add 
      //     all track states except the old one at the IP ....
      TrackStateVec tsv  = trk->trackStates() ;
      trk->trackStates().clear() ;
      
      trk->addTrackState( tsi ) ;
      
      for( int i=0, N=tsv.size() ; i<N ; ++i ){

	if( tsv[i]->getLocation() == lcio::TrackState::AtIP ) {

	  delete  tsv[i] ;

	}else{

	  trk->addTrackState( tsv[i] ) ;
	}
      } //-----------------------------------------------------------------



      trk->setChi2( chi2 + initial_chi2 ) ;
      trk->setNdf(  ndf  + initial_ndf  ) ;
    } 
  }
}

 /*************************************************************************************************/

void ClupatraProcessor::computeTrackInfo(  lcio::Track* lTrk  ){
  
  if( ! lTrk->ext<TrackInfo>() )
    lTrk->ext<TrackInfo>() =  new TrackInfoStruct ;

  float r_inner = _padLayout->getPlaneExtent()[0] ;
  float r_outer = _padLayout->getPlaneExtent()[1] ;
  float driftLength = _gearTPC->getMaxDriftLength() ;

  // compute z-extend of this track segment
  const lcio::TrackerHitVec& hv = lTrk->getTrackerHits() ;
  
  float zMin =  1e99 ;
  float zMax = -1e99 ;
  float zAvg =  0. ;
  
  if( hv.size() >  1 ) {
    zMin = hv[            0  ]->getPosition()[2] ;
    zMax = hv[ hv.size() -1  ]->getPosition()[2] ;
    zAvg = ( zMax + zMin ) / 2. ;
  }
  
  if( zMin > zMax ){ // swap 
    float d = zMax ;
    zMax = zMin ;
    zMin = d  ;
  }
  
  const lcio::TrackState* tsF = lTrk->getTrackState( lcio::TrackState::AtFirstHit  ) ;
  const lcio::TrackState* tsL = lTrk->getTrackState( lcio::TrackState::AtLastHit  ) ;
  
  // protect against bad tracks 
  if(  tsF == 0 ) return ;
  if(  tsL == 0 ) return ;
  
  gear::Vector3D fhPos( tsF->getReferencePoint() ) ;
  gear::Vector3D lhPos( tsL->getReferencePoint() ) ;
  

  TrackInfoStruct* ti = lTrk->ext<TrackInfo>() ;

  ti->startsInner =  std::abs( fhPos.rho() - r_inner )     <  _trackStartsInnerDist ;        // first hit close to inner field cage 
  ti->isCentral   =  std::abs( lhPos.rho() - r_outer )     <  _trackEndsOuterCentralDist ;   // last hit close to outer field cage
  ti->isForward   =  driftLength - std::abs( lhPos.z() )   <  _trackEndsOuterForwardDist  ;  // last hitclose to endcap
  ti->isCurler    =  std::abs( tsF->getOmega() )           >  _trackIsCurlerOmega  ;         // curler segment ( r <~ 1m )
  
  ti->zMin = zMin ;
  ti->zMax = zMax ;
  ti->zAvg = zAvg ;

}


/*************************************************************************************************/
void ClupatraProcessor::check( LCEvent * evt ) { 
  




  /*************************************************************************************************/
}



//====================================================================================================

void ClupatraProcessor::end(){ 
  
  streamlog_out( MESSAGE )  << "ClupatraProcessor::end()  " << name() 
			    << " processed " << _nEvt << " events in " << _nRun << " runs "
			    << std::endl ;
  
}


//====================================================================================================
