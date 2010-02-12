#include "ClupatraProcessor.h"

#include <time.h>
#include <vector>
#include <map>
#include <algorithm>

//---- MarlinUtil 
#include "NNClusters.h"
#include "ClusterShapes.h"

//---- LCIO ---
#include "IMPL/LCCollectionVec.h"
#include "IMPL/TrackImpl.h"
#include "IMPL/TrackerHitImpl.h"
#include "EVENT/SimTrackerHit.h"
//#include "UTIL/LCTypedVector.h"

//---- GEAR ----
#include "marlin/Global.h"
#include "gear/GEAR.h"
#include "gear/TPCParameters.h"
#include "gear/PadRowLayout2D.h"
#include "gear/BField.h"

#include "LCIterator.h"

using namespace lcio ;
using namespace marlin ;

// helper class to assign additional parameters to TrackerHits
struct HitInfoStruct{
  HitInfoStruct() :layerID(-1), usedInTrack(false) {}
  int layerID ;
  bool usedInTrack ;
} ;
struct HitInfo : LCOwnedExtension<HitInfo,HitInfoStruct> {} ;


/** Simple predicate class for applying an r cut to the objects of type T.
 *  Requires float T::getR().
 */
template <class T>
class RCut{
public:
  RCut( double rcut ) : _rcut( rcut ) {}  
  
  bool operator() (T* hit) {  
    return   std::sqrt( hit->getPosition()[0]*hit->getPosition()[0] +
			hit->getPosition()[1]*hit->getPosition()[1] )   > _rcut ; 
  }
protected:
  RCut() {} ;
  double _rcut ;
} ;
//---------------------------------------------------------------------------------
struct ClusterSegment{
  double X ;
  double Y ;
  double R ;
  double ChiS ;
  double ZMin ;
  double ZMax ;
  GenericCluster<TrackerHit>* Cluster ;
  void dump(){
    std::cout << " ----- cluster segment: "
	      << " R " << this->R 
	      << "\t" << "X: " << this->X 
	      << "\t" << "Y: " << this->Y
	      << "\t" << "ChiS: " << this->ChiS 
	      << "\t" << "ZMin: " << this->ZMin
	      << "\t" << "ZMax: " << this->ZMax
	      << std::endl ;
  }
};

ClusterSegment* fitCircle(GenericCluster<TrackerHit>* c) ;

/** Helper class that does a 2d circle fit on cluster segemts */
struct CircleFitter{
    ClusterSegment* operator() (GenericCluster<TrackerHit>* c) {  
      ClusterSegment* s =  fitCircle( c ) ;
      
      return s ;
      //return fitCircle( c ) ;
  }
};

struct SortSegmentsWRTRadius {
  bool operator() (const ClusterSegment* c1, const ClusterSegment* c2) {
    return c1->R < c2->R ;
  }
};

class SegmentDistance{
  typedef ClusterSegment HitClass ;
  typedef double PosType ;
public:

  /** Required typedef for cluster algorithm 
   */
  typedef HitClass hit_type ;

  /** C'tor takes merge distance */
  SegmentDistance(float dCut) : _dCutSquared( dCut*dCut ) , _dCut(dCut){} 

  /** Merge condition: ... */
  inline bool mergeHits( GenericHit<HitClass>* h0, GenericHit<HitClass>* h1){
    
    if( std::abs( h0->Index0 - h1->Index0 ) > 1 ) return false ;

    double r1 = h0->first->R ;
    double r2 = h1->first->R ;
    double x1 = h0->first->X ;
    double x2 = h1->first->X ;
    double y1 = h0->first->Y ;
    double y2 = h1->first->Y ;

//     //------- don't merge hits from same layer !
//     if( h0->first->ext<HitInfo>()->layerID == h1->first->ext<HitInfo>()->layerID )
//       return false ;
    double dr = 2. * std::abs( r1 -r2 ) / (r1 + r2 ) ; 
    
    double distMS = ( x1 - x2 ) * ( x1 - x2 ) + ( y1 - y2 ) * ( y1 - y2 ) ;
 

    return ( dr < 0.1 && distMS < _dCutSquared ) ;
 }
  
protected:
  float _dCutSquared ;
  float _dCut ;
} ;
//---------------------------------------------------------------------------------

/** Predicate class for identifying clusters with duplicate pad rows - returns true
 * if the fraction of duplicate hits is larger than 'fraction'.
 */
struct DuplicatePadRows{

  unsigned _N ;
  float _f ; 
  DuplicatePadRows(unsigned nPadRows, float fraction) : _N( nPadRows), _f( fraction )  {}

  bool operator()(const GenericCluster<TrackerHit>* cl) const {
 
    // check for duplicate layer numbers
    std::vector<int> hLayer( _N )  ; 
    typedef GenericCluster<TrackerHit>::const_iterator IT ;

    unsigned nHit = 0 ;
    for(IT it=cl->begin() ; it != cl->end() ; ++it ) {
      TrackerHit* th = (*it)->first ;
      ++ hLayer[ th->ext<HitInfo>()->layerID ]   ;
      ++ nHit ;
    } 
    unsigned nDuplicate = 0 ;
    for(unsigned i=0 ; i < _N ; ++i ) {
      if( hLayer[i] > 1 ) 
	++nDuplicate ;
    }
    return double(nDuplicate)/nHit > _f ;
  }
};
//---------------------------------------------------------------------------------

/** Predicate class for 'distance' of NN clustering.
 */
//template <class HitClass, typename PosType > 
class HitDistance{
  typedef TrackerHit HitClass ;
  typedef double PosType ;
public:

  /** Required typedef for cluster algorithm 
   */
  typedef HitClass hit_type ;

  /** C'tor takes merge distance */
  HitDistance(float dCut) : _dCutSquared( dCut*dCut ) , _dCut(dCut)  {} 


  /** Merge condition: true if distance  is less than dCut given in the C'tor.*/ 
  inline bool mergeHits( GenericHit<HitClass>* h0, GenericHit<HitClass>* h1){
    
    if( std::abs( h0->Index0 - h1->Index0 ) > 1 ) return false ;

    const PosType* pos0 =  h0->first->getPosition() ;
    const PosType* pos1 =  h1->first->getPosition() ;
    
    //------- don't merge hits from same layer !
    if( h0->first->ext<HitInfo>()->layerID == h1->first->ext<HitInfo>()->layerID )
      return false ;

    return 
      ( pos0[0] - pos1[0] ) * ( pos0[0] - pos1[0] ) +
      ( pos0[1] - pos1[1] ) * ( pos0[1] - pos1[1] ) +
      ( pos0[2] - pos1[2] ) * ( pos0[2] - pos1[2] )   
      < _dCutSquared ;
  }
  
protected:
  HitDistance() ;
  float _dCutSquared ;
  float _dCut ;
} ;


template <class T>
struct LCIOTrack{
  
  lcio::Track* operator() (GenericCluster<T>* c) {  
    
    TrackImpl* trk = new TrackImpl ;
    
    double e = 0.0 ;
    int nHit = 0 ;
    for( typename GenericCluster<T>::iterator hi = c->begin(); hi != c->end() ; hi++) {
      
      trk->addHit(  (*hi)->first ) ;
      e += (*hi)->first->getdEdx() ;
      nHit++ ;
    }

   
    trk->setdEdx( e/nHit ) ;
   
    // FIXME - these are no meaningfull tracks - just a test for clustering tracker hits
    
    return trk ;
  }

} ;



ClupatraProcessor aClupatraProcessor ;


ClupatraProcessor::ClupatraProcessor() : Processor("ClupatraProcessor") {
  
  // modify processor description
  _description = "ClupatraProcessor : simple nearest neighbour clustering" ;
  
  
  StringVec colDefault ;
  colDefault.push_back("AllTPCTrackerHits" ) ;

  registerInputCollections( LCIO::TRACKERHIT,
			   "HitCollections" , 
			   "Name of the input collections"  ,
			   _colNames ,
			   colDefault ) ;
  
  registerOutputCollection( LCIO::TRACK,
			    "OutputCollection" , 
			    "Name of the output collections"  ,
			    _outColName ,
			    std::string("CluTracks" ) ) ;
  
  
  registerProcessorParameter( "DistanceCut" , 
			      "Cut for distance between hits in mm"  ,
			      _distCut ,
			      (float) 40.0 ) ;
  
  registerProcessorParameter( "MinimumClusterSize" , 
			      "minimum number of hits per cluster"  ,
			      _minCluSize ,
			      (int) 3) ;
  

  registerProcessorParameter( "DuplicatePadRowFraction" , 
			      "allowed fraction of hits in same pad row per track"  ,
			       _duplicatePadRowFraction,
			      (float) 0.02 ) ;

  registerProcessorParameter( "RCut" , 
			      "Cut for r_min in mm"  ,
			      _rCut ,
			      (float) 0.0 ) ;
  
}


void ClupatraProcessor::init() { 

  // usually a good idea to
  printParameters() ;

  _nRun = 0 ;
  _nEvt = 0 ;
}

void ClupatraProcessor::processRunHeader( LCRunHeader* run) { 

  _nRun++ ;
} 

void ClupatraProcessor::processEvent( LCEvent * evt ) { 

  clock_t start =  clock() ; 

  LCCollectionVec* lcioTracks = new LCCollectionVec( LCIO::TRACK )  ;
  
  GenericHitVec<TrackerHit> h ;
  
  GenericClusterVec<TrackerHit> cluList ;
  
  RCut<TrackerHit> rCut( _rCut ) ;
  
  ZIndex<TrackerHit,80> zIndex( -2750. , 2750. ) ; 
  
  //  NNDistance< TrackerHit, double> dist( _distCut )  ;
  HitDistance dist( _distCut ) ;
  
  LCIOTrack<TrackerHit> converter ;
  
  const gear::TPCParameters& gearTPC = Global::GEAR->getTPCParameters() ;
  const gear::PadRowLayout2D& padLayout = gearTPC.getPadLayout() ;
  unsigned nPadRows = padLayout.getNRows() ;

  // create a vector of generic hits from the collection applying a cut on r_min
  for( StringVec::iterator it = _colNames.begin() ; it !=  _colNames.end() ; it++ ){  
    
    LCCollection* col =  evt->getCollection( *it )  ; 
    
    
    //--- assign the layer number to the TrackerHits
    
    int nHit = col->getNumberOfElements() ;
    for(int i=0 ; i < nHit ; ++i ) {
      
      TrackerHitImpl* th = (TrackerHitImpl*) col->getElementAt(i) ;
      gear::Vector3D v( th->getPosition()[0],th->getPosition()[1], 0 ) ; 
      int padIndex = padLayout.getNearestPad( v.rho() , v.phi() ) ;
      
      th->ext<HitInfo>() = new HitInfoStruct ;

      th->ext<HitInfo>()->layerID = padLayout.getRowNumber( padIndex ) ;
      
//       //--- for fixed sized rows this would also work...
//       float rMin = padLayout.getPlaneExtent()[0] ;
//       float rMax = padLayout.getPlaneExtent()[1] ;
//       float nRow  = padLayout.getNRows() ;
//       int lCheck =  ( v.rho() - rMin ) / ((rMax - rMin ) /nRow ) ;

//       streamlog_out( DEBUG ) << " layerID : " << th->ext<HitInfo>()->layerID 
// 			     << " r: " << v.rho() 
// 			     << " lCheck : " << lCheck 
// 			     << " phi : " << v.phi()
// 			     << " rMin : " << rMin 
// 			     << " rMax : " << rMax 
// 			     << std::endl ;

    } //-------------------- end assign layernumber ---------
    
    addToGenericHitVec( h , col , rCut , zIndex ) ;
  }  

  // cluster the hits with a nearest neighbour condition
  cluster( h.begin() , h.end() , std::back_inserter( cluList )  , &dist , _minCluSize ) ;
  
  streamlog_out( DEBUG ) << "   ***** clusters: " << cluList.size() << std::endl ; 

  // find 'odd' clusters that have duplicate hits in pad rows
  GenericClusterVec<TrackerHit> ocs ;


 //  typedef GenericClusterVec<TrackerHit>::iterator GCVI ;

//   for( GCVI it = cluList.begin() ; it != cluList.end() ; ++it ){
//     std::cout << " *** cluster :" << (*it)->ID 
// 	      << " size() :" << (*it)->size() 
// 	      << " at : " << *it << std::endl ;
//   }
  //  GCVI remIt =
//     std::remove_copy_if( cluList.begin(), cluList.end(), std::back_inserter( ocs ) ,  DuplicatePadRows() ) ;

  split_list( cluList, std::back_inserter(ocs),  DuplicatePadRows( nPadRows, _duplicatePadRowFraction  ) ) ;

//   for( GCVI it = cluList.begin() ; it != cluList.end() ; ++it ){
//     std::cout << " +++ cluster :" << (*it)->ID 
// 	      << " size() :" << (*it)->size() 
// 	      << " at : " << *it << std::endl ;
//   }

//   for( GCVI it = cluList.begin() ; it != cluList.end() ; ++it ){
//     if( (*it)->size() > 20 ){
//       ocs.splice( ocs.begin() , cluList , it  );
//     }
//   }


  LCCollectionVec* oddCol = new LCCollectionVec( LCIO::TRACK ) ;
  std::transform( ocs.begin(), ocs.end(), std::back_inserter( *oddCol ) , converter ) ;
  evt->addCollection( oddCol , "OddClu_1" ) ;


  streamlog_out( DEBUG ) << "   ***** clusters: " << cluList.size() 
			 << "   ****** oddClusters " << ocs.size() 
			 << std::endl ; 



//   //-------------------- split up cluster with duplicate rows 
//   // NOTE: a 'simple' combinatorical Kalman filter would do the trick here ...

  GenericClusterVec<TrackerHit> sclu ; // new split clusters

  std::vector< GenericHit<TrackerHit>* > oddHits ;
  oddHits.reserve( h.size() ) ;

  typedef GenericClusterVec<TrackerHit>::iterator GCVI ;

  for( GCVI it = ocs.begin() ; it != ocs.end() ; ++it ){
    (*it)->takeHits( std::back_inserter( oddHits )  ) ;
    delete (*it) ;
  }
  ocs.clear() ;

  int _nRowForSplitting = 20 ; //FIXME:  make proc param
  // reset the hits index to row ranges for reclustering
  unsigned nOddHits = oddHits.size() ;
  for(unsigned i=0 ; i< nOddHits ; ++i){
    int layer =  oddHits[i]->first->ext<HitInfo>()->layerID  ;
    oddHits[i]->Index0 =   2 * int( layer / _nRowForSplitting ) ;
  }

  //----- recluster in pad row ranges
  cluster( oddHits.begin(), oddHits.end() , std::back_inserter( sclu ), &dist , _minCluSize ) ;

  LCCollectionVec* oddCol2 = new LCCollectionVec( LCIO::TRACK ) ;
  std::transform( sclu.begin(), sclu.end(), std::back_inserter( *oddCol2 ) , converter ) ;
  evt->addCollection( oddCol2 , "OddClu_2" ) ;


  streamlog_out( DEBUG ) << "   ****** oddClusters fixed" << sclu.size() 
			 << std::endl ; 
 
 //--------- remove pad row range clusters where merge occured 
  split_list( sclu, std::back_inserter(ocs), DuplicatePadRows( nPadRows, _duplicatePadRowFraction  ) ) ;


  LCCollectionVec* oddCol3 = new LCCollectionVec( LCIO::TRACK ) ;
  std::transform( ocs.begin(), ocs.end(), std::back_inserter( *oddCol3 ) , converter ) ;
  evt->addCollection( oddCol3 , "OddClu_3" ) ;
  oddHits.clear() ;

  //----------------end  split up cluster with duplicate rows 




  // --- recluster the good clusters w/ all pad rows

  for( GCVI it = sclu.begin() ; it != sclu.end() ; ++it ){
    (*it)->takeHits( std::back_inserter( oddHits )  ) ;
    delete (*it) ;
  }
  sclu.clear() ;

  //   reset the index for 'good' hits coordinate again...
  nOddHits = oddHits.size() ;
  for(unsigned i=0 ; i< nOddHits ; ++i){
    oddHits[i]->Index0 = zIndex ( oddHits[i]->first ) ;
  }

  cluster( oddHits.begin(), oddHits.end() , std::back_inserter( sclu ), &dist , _minCluSize ) ;

  LCCollectionVec* oddCol4 = new LCCollectionVec( LCIO::TRACK ) ;
  std::transform( sclu.begin(), sclu.end(), std::back_inserter( *oddCol4 ) , converter ) ;
  evt->addCollection( oddCol4 , "OddClu_4" ) ;

  // --- end recluster the good clusters w/ all pad rows

  //TO DO:
  // ---- check again for duplicate pad rows 
  //  -> can occur if tracks merged at pad row range border .....

  // merge the good clusters to final list
  cluList.merge( sclu ) ;
  

  //============ now try reclustering with circle segments ======================
  GenericClusterVec<TrackerHit> mergedClusters ; // new split clusters

  std::list< ClusterSegment* > segs ;
  
  std::transform( cluList.begin(), cluList.end(), std::back_inserter( segs ) , CircleFitter() ) ;

  segs.sort( SortSegmentsWRTRadius() ) ;

  std::for_each( segs.begin(), segs.end() , std::mem_fun( &ClusterSegment::dump )  ) ; 

  GenericHitVec< ClusterSegment > segclu ;
  GenericClusterVec< ClusterSegment > mergedSegs ;
  
  double _circleCenterDist = 30. ; // FIXME make proc para
  SegmentDistance segDist( _circleCenterDist )  ; 
 
  addToGenericHitVec( segclu , segs.begin(), segs.end(), AllwaysTrue() , NullIndex() ) ;

  cluster( segclu.begin(), segclu.end(), std::back_inserter( mergedSegs ), &segDist, 2 ) ;
 
  // now merge the corresponding hit lists...

  for( GenericClusterVec< ClusterSegment >::iterator it = mergedSegs.begin() ;
       it != mergedSegs.end() ; ++it){

    std::cout <<  " ===== merged segements =========" << std::endl ;

    GenericCluster<ClusterSegment>::iterator si = (*it)->begin() ;
   
    ClusterSegment* merged = (*si)->first  ;

    merged->dump() ;

    ++si ;

    while( si != (*it)->end() ){

      ClusterSegment* seg = (*si)->first ;
      seg->dump() ;

      merged->Cluster->merge(  *seg->Cluster ) ;

      ++si ;
    } 
    mergedClusters.push_back( merged->Cluster ) ; 

    std::cout <<  " ===== " << std::endl ;

  }

  LCCollectionVec* mergeCol = new LCCollectionVec( LCIO::TRACK ) ;
  std::transform( mergedClusters.begin(), mergedClusters.end(), std::back_inserter( *mergeCol ) , converter ) ;
  evt->addCollection( mergeCol , "MergedTrackSegements" ) ;
  mergedClusters.clear() ;

  //============ end reclustering with circle segments ===========================

 



  // create "lcio::Tracks" from the clustered TrackerHits
   std::transform( cluList.begin(), cluList.end(), std::back_inserter( *lcioTracks ) , converter ) ;
   evt->addCollection( lcioTracks , _outColName ) ;
  
  
  _nEvt ++ ;

  clock_t end = clock () ; 
  
  streamlog_out( DEBUG )  << "---  clustering time: " 
			  <<  double( end - start ) / double(CLOCKS_PER_SEC) << std::endl  ;
}



void ClupatraProcessor::check( LCEvent * evt ) { 

  bool checkForDuplicatePadRows =  true ;
  bool checkForMCTruth =  true ;

  streamlog_out( MESSAGE ) <<  " check called.... " << std::endl ;

  const gear::TPCParameters& gearTPC = Global::GEAR->getTPCParameters() ;
  const gear::PadRowLayout2D& pL = gearTPC.getPadLayout() ;


  //====================================================================================
  // check for duplicate padRows 
  //====================================================================================

  if( checkForDuplicatePadRows ) {

    LCCollectionVec* oddCol = new LCCollectionVec( LCIO::TRACK ) ;
    oddCol->setSubset( true ) ;
    // try iterator class ...
    LCIterator<Track> trIt( evt, _outColName ) ;
    while( Track* tr = trIt.next()  ){
      
      // check for duplicate layer numbers
      std::vector<int> hitsInLayer( pL.getNRows() ) ; 
      const TrackerHitVec& thv = tr->getTrackerHits() ;
      typedef TrackerHitVec::const_iterator THI ;
      for(THI it = thv.begin() ; it  != thv.end() ; ++it ) {
	TrackerHit* th = *it ;
	++ hitsInLayer.at( th->ext<HitInfo>()->layerID )   ;
      } 
      unsigned nHit = thv.size() ;
      unsigned nDouble = 0 ;
      for(unsigned i=0 ; i < hitsInLayer.size() ; ++i ) {
	if( hitsInLayer[i] > 1 ) 
	  ++nDouble ;
      }
      if( double(nDouble) / nHit > _duplicatePadRowFraction ){
	//if( nDouble  > 0){
	streamlog_out( MESSAGE ) << " oddTrackCluster found with "<< 100. * double(nDouble) / nHit 
				 << "% of double hits " << std::endl ;
	oddCol->addElement( tr ) ;
      }
    }
    evt->addCollection( oddCol , "OddCluTracks" ) ;
  }
  //====================================================================================
  // check Monte Carlo Truth via SimTrackerHits 
  //====================================================================================

  if( checkForMCTruth ) {
 

    LCCollectionVec* oddCol = new LCCollectionVec( LCIO::TRACK ) ;
    oddCol->setSubset( true ) ;

    LCIterator<Track> trIt( evt, _outColName ) ;
    //    LCIterator<Track> trIt( evt, "TPCTracks" ) ;
    while( Track* tr = trIt.next()  ){

      typedef std::map< MCParticle* , unsigned > MCPMAP ;
      MCPMAP mcpMap ;

      const TrackerHitVec& thv = tr->getTrackerHits() ;
      typedef TrackerHitVec::const_iterator THI ;

      for(THI it = thv.begin() ; it  != thv.end() ; ++it ) {

	TrackerHit* th = *it ;
	// FIXME:
	// we know that the digitizer puts the sim hit into the raw hit pointer
	// but of course the proper way is to go through the LCRelation ...
	SimTrackerHit* sh = (SimTrackerHit*) th->getRawHits()[0] ;
	MCParticle* mcp = sh->getMCParticle() ;
	mcpMap[ mcp ]++ ;

      } 

      unsigned nHit = thv.size() ;
      unsigned maxHit = 0 ; 
      for( MCPMAP::iterator it= mcpMap.begin() ;
	   it != mcpMap.end() ; ++it ){
	if( it->second  > maxHit ){
	  maxHit = it->second ;
	}
      }

      if( double(maxHit) / nHit < 0.99 ){ // What is acceptable here ???
	//if( nDouble  > 0){
	streamlog_out( MESSAGE ) << " oddTrackCluster found with only "
				 << 100.*double(maxHit)/nHit 
				 << "% of hits  form one MCParticle " << std::endl ;
	oddCol->addElement( tr ) ;
      }
    }
    evt->addCollection( oddCol , "OddMCPTracks" ) ;
  }
  //====================================================================================

}


void ClupatraProcessor::end(){ 
  
  //   std::cout << "ClupatraProcessor::end()  " << name() 
  // 	    << " processed " << _nEvt << " events in " << _nRun << " runs "
  // 	    << std::endl ;

}



ClusterSegment* fitCircle(GenericCluster<TrackerHit>* c){

  // code copied from GenericViewer
  // should be replaced by a more efficient circle fit...
  
  //	    for (int iclust(0); iclust < nelem; ++iclust) {
  int nhits = (int) c->size() ;
  float * ah = new float[nhits];
  float * xh = new float[nhits];
  float * yh = new float[nhits];
  float * zh = new float[nhits];
  float zmin = 1.0E+10;
  float zmax = -1.0E+10;

  int ihit = -1 ;
  typedef GenericCluster<TrackerHit>::iterator IT ;
  for( IT it=c->begin() ; it != c->end() ; ++it ){
    
    ++ihit ;
    TrackerHit * hit = (*it)->first ;
    float x = (float)hit->getPosition()[0];
    float y = (float)hit->getPosition()[1];
    float z = (float)hit->getPosition()[2];
    ah[ihit] = 1.0;
    xh[ihit] = x;
    yh[ihit] = y;
    zh[ihit] = z;
    if (z < zmin)
      zmin = z;
    if (z > zmax)
      zmax = z;
  } 	
  //  ClusterShapes * shapes = new ClusterShapes(nhits,ah,xh,yh,zh);
  ClusterShapes shapes(nhits,ah,xh,yh,zh) ;
  float zBegin, zEnd;
  if (fabs(zmin)<fabs(zmax)) {
    zBegin = zmin;
    zEnd   = zmax;
  }
  else {
    zBegin = zmax;
    zEnd   = zmin;
  }
  //  float signPz = zEnd - zBegin;		
  //  float dz = (zmax - zmin) / 500.;
  float par[5];
  float dpar[5];
  float chi2;
  float distmax;
  shapes.FitHelix(500, 0, 1, par, dpar, chi2, distmax);

  ClusterSegment* segment = new ClusterSegment ;
  segment->X = par[0];
  segment->Y = par[1];
  segment->R = par[2];
  segment->ChiS = chi2 ;
  segment->ZMin = zmin ;
  segment->ZMax = zmax ;
  segment->Cluster = c ;
 
//   float x0 = par[0];
//   float y0 = par[1];
//   float r0 = par[2];
//   float bz = par[3];
//   float phi0 = par[4];

  //   HelixClass * helix = new HelixClass();
  //   helix->Initialize_BZ(x0, y0, r0, 
  // 		       bz, phi0, _bField,signPz,
  // 		       zBegin);
  //   std::cout << "Track " << iclust << " ;  d0 = " << helix->getD0()
  // 	    << " ; z0 = " << helix->getZ0() 
  // 	    << " ; omega = " << helix->getOmega() 
  // 	    << " ; tanlam = " << helix->getTanLambda()
  // 	    << " ; tan(phi0) = " << tan(helix->getPhi0()) 
  // 	    << std::endl;		
  //   if (chi2 > 0. && chi2 < 10.) {
  //     for (int iz(0); iz < 500; ++iz) {
  //       float z1 = zmin + iz*dz;
  //       float z2 = z1 + dz;
  //       float x1 = x0 + r0*cos(bz*z1+phi0);
  //       float y1 = y0 + r0*sin(bz*z1+phi0);
  //       float x2 = x0 + r0*cos(bz*z2+phi0);
  //       float y2 = y0 + r0*sin(bz*z2+phi0);			
  //       ced_line(x1,y1,z1,x2,y2,z2,_layerTracks<<CED_LAYER_SHIFT,1,0xFFFFFF);
  
  //     }		
  //   }
  //  delete shapes;
  //  delete helix;
  delete[] xh;
  delete[] yh;
  delete[] zh;
  delete[] ah;

  return segment ;
}
