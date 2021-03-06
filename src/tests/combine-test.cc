#include "sysexits.h"

#include "alfalfa_video.hh"
#include "filesystem.hh"
#include "tracking_player.hh"

using namespace std;

/* Simple combine test. Imports the same video twice, and combines them.
   Checks to make sure entries in the raster list, quality db and frame db
   aren't replicated. Also checks to make sure track db has two distinct tracks. */
int alfalfa_combine_test_double_import( string ivf_file_path, string alfalfa_video_dir ) {
  bool found;

  AlfalfaVideo alfalfa_video( alfalfa_video_dir, OpenMode::READ );

  RasterList & raster_list = alfalfa_video.raster_list();
  QualityDB & quality_db = alfalfa_video.quality_db();
  FrameDB & frame_db = alfalfa_video.frame_db();

  TrackingPlayer player( ivf_file_path );
  size_t frame_index = 0;

  while ( not player.eof() ) {
    FrameInfo next_frame( player.serialize_next().first );

    const size_t raster = next_frame.target_hash().output_hash;
    const size_t approximate_raster = raster;

    if ( next_frame.target_hash().shown ) {
      // Check raster_list
      if ( not raster_list.has( raster ) )
        return 1;

      // Check quality db
      found = false;
      auto it_orig =
        quality_db.search_by_original_raster( raster );
      QualityDBCollectionByOriginalRaster::iterator it_begin_orig = it_orig.first;
      QualityDBCollectionByOriginalRaster::iterator it_end_orig = it_orig.second;
      while ( it_begin_orig != it_end_orig ) {
        if ( it_begin_orig->original_raster != raster )
          return 1;
        if ( it_begin_orig->approximate_raster == approximate_raster ) {
          if ( it_begin_orig->quality == 1.0 )
            found = true;
        }
        it_begin_orig++;
      }
      if ( !found )
        return 1;

      found = false;
      auto it_approx =
        quality_db.search_by_approximate_raster( approximate_raster );
      QualityDBCollectionByApproximateRaster::iterator it_begin_approx = it_approx.first;
      QualityDBCollectionByApproximateRaster::iterator it_end_approx = it_approx.second;
      while ( it_begin_approx != it_end_approx ) {
        if ( it_begin_approx->approximate_raster != approximate_raster )
          return 1;
        if ( it_begin_approx->original_raster == raster ) {
          if ( it_begin_approx->quality == 1.0 )
            found = true;
        }
        it_begin_approx++;
      }
      if ( !found )
        return 1;
    }

    // Check frame db
    found = false;
    auto it_output_hash =
      frame_db.search_by_output_hash( raster );
    FrameDataSetCollectionByOutputHash::iterator it_begin_output_hash = it_output_hash.first;
    FrameDataSetCollectionByOutputHash::iterator it_end_output_hash = it_output_hash.second;
    while ( it_begin_output_hash != it_end_output_hash ) {
      if ( it_begin_output_hash->source_hash() == next_frame.source_hash() and
           it_begin_output_hash->target_hash() == next_frame.target_hash() )
        found = true;
      it_begin_output_hash++;
    }
    if ( !found )
      return 1;

    // Check track db
    bool found;

    found = false;
    pair<TrackDBIterator, TrackDBIterator> it_ids1 =
      alfalfa_video.get_frames( 0 );
    TrackDBIterator it_begin_ids1 = it_ids1.first;
    TrackDBIterator it_end_ids1 = it_ids1.second;
    size_t local_frame_index1 = 0;
    while ( it_begin_ids1 != it_end_ids1 ) {
      FrameInfo frame_info = *it_begin_ids1;
      if ( local_frame_index1 == frame_index and
           frame_info.source_hash() == next_frame.source_hash() and
           frame_info.target_hash() == next_frame.target_hash() )
        found = true;
      local_frame_index1++;
      it_begin_ids1++;
    }
    if ( !found ) {
      return 1;
    }

    found = false;
    pair<TrackDBIterator, TrackDBIterator> it_ids2 =
      alfalfa_video.get_frames( 1 );
    TrackDBIterator it_begin_ids2 = it_ids2.first;
    TrackDBIterator it_end_ids2 = it_ids2.second;
    size_t local_frame_index2 = 0;
    while ( it_begin_ids2 != it_end_ids2 ) {
      FrameInfo frame_info = *it_begin_ids2;
      if ( local_frame_index2 == frame_index and
           frame_info.source_hash() == next_frame.source_hash() and
           frame_info.target_hash() == next_frame.target_hash() )
        found = true;
      local_frame_index2++;
      it_begin_ids2++;
    }
    if ( !found ) {
      return 1;
    }

    frame_index++;
  }

  return 0;
}

int main(int argc, char const *argv[]) {
  if ( argc != 3 ) {
    cerr << "usage: combine-test <ivf-file> <alfalfa-video-dir>" << endl;
    return 1;
  }

  string ivf_file_path( argv[ 1 ] );
  string alfalfa_video_dir( argv[ 2 ] );

  return alfalfa_combine_test_double_import( ivf_file_path, alfalfa_video_dir );
}
