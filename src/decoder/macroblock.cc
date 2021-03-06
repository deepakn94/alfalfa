#include "macroblock.hh"
#include "scorer.hh"

#include "tokens.cc"
#include "transform.cc"
#include "prediction.cc"
#include "quantization.cc"
#include "tree.cc"

using namespace std;

static bmode implied_subblock_mode( const mbmode y_mode )
{
  switch ( y_mode ) {
  case DC_PRED: return B_DC_PRED;
  case V_PRED:  return B_VE_PRED;
  case H_PRED:  return B_HE_PRED;
  case TM_PRED: return B_TM_PRED;
  default: throw LogicError();
  }
}

template <class FrameHeaderType, class MacroblockHeaderType>
Macroblock<FrameHeaderType, MacroblockHeaderType>::Macroblock( const typename TwoD< Macroblock >::Context & c,
							       BoolDecoder & data,
							       const FrameHeaderType & frame_header,
							       const ProbabilityArray< num_segments > & mb_segment_tree_probs,
							       const ProbabilityTables & probability_tables,
							       const Optional< ContinuationHeader > & continuation_header,
							       TwoD< Y2Block > & frame_Y2,
							       TwoD< YBlock > & frame_Y,
							       TwoD< UVBlock > & frame_U,
							       TwoD< UVBlock > & frame_V )
  : context_( c ),
    segment_id_update_( frame_header.update_segmentation.initialized()
			and frame_header.update_segmentation.get().update_mb_segmentation_map,
			data, mb_segment_tree_probs ),
    segment_id_( 0 ),
    mb_skip_coeff_( frame_header.prob_skip_false.initialized(),
		    data, frame_header.prob_skip_false.get_or( 0 ) ),
    header_( data, frame_header ),
    continuation_( continuation_header.initialized()
		   and inter_coded()
		   and continuation_header.get().is_missing( header_.reference() ) ),
    loopfilter_skip_subblock_edges_( continuation_, data ),
    Y2_( frame_Y2.at( c.column, c.row ) ),
    Y_( frame_Y, c.column * 4, c.row * 4 ),
    U_( frame_U, c.column * 2, c.row * 2 ),
    V_( frame_V, c.column * 2, c.row * 2 )
{
  decode_prediction_modes( data, probability_tables );
}

template<>
InterFrameMacroblock::Macroblock( const typename TwoD< Macroblock >::Context & c,
				  const TwoD< InterFrameMacroblock > & old_macroblocks,
	      			  TwoD< Y2Block > & frame_Y2,
	      			  TwoD< YBlock > & frame_Y,
	      			  TwoD< UVBlock > & frame_U,
	      			  TwoD< UVBlock > & frame_V )
  : context_( c ),
    segment_id_update_( old_macroblocks.at( c.column, c.row ).segment_id_update_ ),
    segment_id_( old_macroblocks.at( c.column, c.row ).segment_id_ ),
    mb_skip_coeff_( old_macroblocks.at( c.column, c.row ).mb_skip_coeff_ ),
    header_( old_macroblocks.at( c.column, c.row ).header_ ),
    continuation_( false ),
    loopfilter_skip_subblock_edges_( old_macroblocks.at( c.column, c.row ).loopfilter_skip_subblock_edges_ ),
    Y2_( frame_Y2.at( c.column, c.row ) ),
    Y_( frame_Y, c.column * 4, c.row * 4 ),
    U_( frame_U, c.column * 2, c.row * 2 ),
    V_( frame_V, c.column * 2, c.row * 2 ),
    has_nonzero_( old_macroblocks.at( c.column, c.row ).has_nonzero_ )
{}

template <class FrameHeaderType, class MacroblockHeaderType>
void Macroblock<FrameHeaderType, MacroblockHeaderType>::update_segmentation( SegmentationMap & mutable_segmentation_map ) {
  /* update persistent segmentation map */
  if ( segment_id_update_.initialized() ) {
    mutable_segmentation_map.at( context_.column, context_.row ) = segment_id_update_.get();
  }

  /* cache segment id of this macroblock*/
  segment_id_ = mutable_segmentation_map.at( context_.column, context_.row );
}

template <>
void KeyFrameMacroblock::decode_prediction_modes( BoolDecoder & data,
						  const ProbabilityTables & )
{
  /* Set Y prediction mode */
  Y2_.set_prediction_mode( Tree< mbmode, num_y_modes, kf_y_mode_tree >( data, kf_y_mode_probs ) );
  Y2_.set_if_coded();

  /* Set subblock prediction modes */
  Y_.forall( [&]( YBlock & block )
	     {
	       if ( Y2_.prediction_mode() == B_PRED ) {
		 const auto above_mode = block.context().above.initialized()
		   ? block.context().above.get()->prediction_mode() : B_DC_PRED;
		 const auto left_mode = block.context().left.initialized()
		   ? block.context().left.get()->prediction_mode() : B_DC_PRED;
		 block.set_Y_without_Y2();
		 block.set_prediction_mode( Tree< bmode, num_intra_b_modes, b_mode_tree >( data,
											   kf_b_mode_probs.at( above_mode ).at( left_mode ) ) );
	       } else {
		 block.set_prediction_mode( implied_subblock_mode( Y2_.prediction_mode() ) );
	       }
	     } );

  /* Set chroma prediction mode */
  U_.at( 0, 0 ).set_prediction_mode( Tree< mbmode, num_uv_modes, uv_mode_tree >( data, kf_uv_mode_probs ) );
}


template <>
void InterFrameMacroblock::set_base_motion_vector( const MotionVector & mv )
{
  Y_.at( 3, 3 ).set_motion_vector( mv );
}

template <>
const MotionVector & InterFrameMacroblock::base_motion_vector( void ) const
{
  return Y_.at( 3, 3 ).motion_vector();
}

template <>
bool KeyFrameMacroblock::inter_coded( void ) const
{
  return false;
}

template <>
bool InterFrameMacroblock::inter_coded( void ) const
{
  return header_.is_inter_mb;
}

void Scorer::add( const uint8_t score, const Optional< const InterFrameMacroblock * > & mb )
{
  if ( mb.initialized() and mb.get()->inter_coded() ) {
    MotionVector mv = mb.get()->base_motion_vector();
    if ( mb.get()->header().motion_vectors_flipped_ != motion_vectors_flipped_ ) {
      mv = -mv;
    }
    add( score, mv );
    if ( mb.get()->y_prediction_mode() == SPLITMV ) {
      splitmv_score_ += score;
    }
  }
}

void Scorer::calculate( void )
{
  if ( scores_.at( 3 ) ) {
    if ( motion_vectors_.at( index_ ) == motion_vectors_.at( 1 ) ) {
      scores_.at( 1 ) += scores_.at( 3 );
    }
  }

  if ( scores_.at( 2 ) > scores_.at( 1 ) ) {
    tie( scores_.at( 1 ), scores_.at( 2 ) ) = make_pair( scores_.at( 2 ), scores_.at( 1 ) );
    tie( motion_vectors_.at( 1 ), motion_vectors_.at( 2 ) )
      = make_pair( motion_vectors_.at( 2 ), motion_vectors_.at( 1 ) );
  }

  if ( scores_.at( 1 ) >= scores_.at( 0 ) ) {
    motion_vectors_.at( 0 ) = motion_vectors_.at( 1 );
  }
}

void MotionVector::clamp( const int16_t to_left, const int16_t to_right,
			  const int16_t to_top, const int16_t to_bottom )
{
  x_ = min( max( x_, to_left ), to_right );
  y_ = min( max( y_, to_top ), to_bottom );
}

MotionVector Scorer::clamp( const MotionVector & mv, const typename TwoD< InterFrameMacroblock >::Context & c )
{
  MotionVector ret( mv );

  const int16_t to_left = -((c.column * 16) << 3) - 128;
  const int16_t to_right = (((c.width - 1 - c.column) * 16) << 3) + 128;
  const int16_t to_top = (-((c.row * 16)) << 3) - 128;
  const int16_t to_bottom = (((c.height - 1 - c.row) * 16) << 3) + 128;

  ret.clamp( to_left, to_right, to_top, to_bottom );

  return ret;
}

/* Taken from libvpx dixie read_mv_component() */
int16_t MotionVector::read_component( BoolDecoder & data,
				      const SafeArray< Probability, MV_PROB_CNT > & component_probs )
{
  enum { IS_SHORT, SIGN, SHORT, BITS = SHORT + 8 - 1, LONG_WIDTH = 10 };
  int16_t x = 0;

  if ( data.get( component_probs.at( IS_SHORT ) ) ) { /* Large */
    for ( uint8_t i = 0; i < 3; i++ ) {
      x += data.get( component_probs.at( BITS + i ) ) << i;
    }

    /* Skip bit 3, which is sometimes implicit */
    for ( uint8_t i = LONG_WIDTH - 1; i > 3; i-- ) {
      x += data.get( component_probs.at( BITS + i ) ) << i;
    }

    if ( !(x & 0xFFF0) or data.get( component_probs.at( BITS + 3 ) ) ) {
      x += 8;
    }
  } else {  /* small */
    x = Tree< int16_t, 8, small_mv_tree >( data, component_probs.slice<SHORT, 7>() );
  }

  x <<= 1; /* must do before it can be negative or
	      -fsanitize complains */

  if ( x && data.get( component_probs.at( SIGN ) ) ) {
    x = -x;
  }

  return x;
}

template <>
void YBlock::read_subblock_inter_prediction( BoolDecoder & data,
					     const MotionVector & best_mv,
					     const SafeArray< SafeArray< Probability, MV_PROB_CNT >, 2 > & motion_vector_probs )
{
  const MotionVector default_mv;

  const MotionVector & left_mv = context().left.initialized() ? context().left.get()->motion_vector() : default_mv;

  const MotionVector & above_mv = context().above.initialized() ? context().above.get()->motion_vector() : default_mv;

  const bool left_is_zero = left_mv.empty();
  const bool above_is_zero = above_mv.empty();
  const bool left_eq_above = left_mv == above_mv;

  uint8_t submv_ref_index = 0;

  if ( left_eq_above and left_is_zero ) {
    submv_ref_index = 4;
  } else if ( left_eq_above ) {
    submv_ref_index = 3;
  } else if ( above_is_zero ) {
    submv_ref_index = 2;
  } else if ( left_is_zero ) {
    submv_ref_index = 1;
  }

  prediction_mode_ = Tree< bmode, num_inter_b_modes, submv_ref_tree >( data,
								       submv_ref_probs2.at( submv_ref_index ) );

  switch ( prediction_mode_ ) {
  case LEFT4X4:
    motion_vector_ = left_mv;
    break;
  case ABOVE4X4:
    motion_vector_ = above_mv;
    break;
  case ZERO4X4: /* zero by default */
    assert( motion_vector_.empty() );
    break;
  case NEW4X4:
    {
      MotionVector new_mv( data, motion_vector_probs );
      new_mv += best_mv;
      motion_vector_ = new_mv;
    }
    break;
  default:
    throw LogicError();
  }
}

MotionVector::MotionVector( BoolDecoder & data,
			    const SafeArray< SafeArray< Probability, MV_PROB_CNT >, 2 > & motion_vector_probs )
  : y_( read_component( data, motion_vector_probs.at( 0 ) ) ),
    x_( read_component( data, motion_vector_probs.at( 1 ) ) )
{}

MotionVector luma_to_chroma( const MotionVector & s1,
			     const MotionVector & s2,
			     const MotionVector & s3,
			     const MotionVector & s4 )
{
  const int16_t x = s1.x() + s2.x() + s3.x() + s4.x();
  const int16_t y = s1.y() + s2.y() + s3.y() + s4.y();

  return MotionVector( x >= 0 ?  (x + 4) >> 3 : -((-x + 4) >> 3),
		       y >= 0 ?  (y + 4) >> 3 : -((-y + 4) >> 3) );
}

template <>
void InterFrameMacroblock::decode_prediction_modes( BoolDecoder & data,
						    const ProbabilityTables & probability_tables )
{
  if ( not inter_coded() ) {
    /* Set Y prediction mode */
    Y2_.set_prediction_mode( Tree< mbmode, num_y_modes, y_mode_tree >( data, probability_tables.y_mode_probs ) );
    Y2_.set_if_coded();

    /* Set subblock prediction modes. Intra macroblocks in interframes are simpler than in keyframes. */
    Y_.forall( [&]( YBlock & block )
	       {
		 if ( Y2_.prediction_mode() == B_PRED ) {
		   block.set_Y_without_Y2();
		   block.set_prediction_mode( Tree< bmode, num_intra_b_modes, b_mode_tree >( data,
											     invariant_b_mode_probs ) );
		 } else {
		   block.set_prediction_mode( implied_subblock_mode( Y2_.prediction_mode() ) );
		 }
	       } );

    /* Set chroma prediction modes */
    U_.at( 0, 0 ).set_prediction_mode( Tree< mbmode, num_uv_modes, uv_mode_tree >( data,
										   probability_tables.uv_mode_probs ) );
  } else {
    /* motion-vector "census" */
    Scorer census( header_.motion_vectors_flipped_ );
    census.add( 2, context_.above );
    census.add( 2, context_.left );
    census.add( 1, context_.above_left );
    census.calculate();

    const auto counts = census.mode_contexts();

    /* census determines lookups into fixed probability table */
    const ProbabilityArray< num_mv_refs > mv_ref_probs = {{ mv_counts_to_probs.at( counts.at( 0 ) ).at( 0 ),
							    mv_counts_to_probs.at( counts.at( 1 ) ).at( 1 ),
							    mv_counts_to_probs.at( counts.at( 2 ) ).at( 2 ),
							    mv_counts_to_probs.at( counts.at( 3 ) ).at( 3 ) }};

    Y2_.set_prediction_mode( Tree< mbmode, num_mv_refs, mv_ref_tree >( data, mv_ref_probs ) );
    Y2_.set_if_coded();

    switch ( Y2_.prediction_mode() ) {
    case NEARESTMV:
      set_base_motion_vector( Scorer::clamp( census.nearest(), context_ ) );
      break;
    case NEARMV:
      set_base_motion_vector( Scorer::clamp( census.near(), context_ ) );
      break;
    case ZEROMV:
      set_base_motion_vector( MotionVector() );
      break;
    case NEWMV:
      {
	MotionVector new_mv( data, probability_tables.motion_vector_probs );
	new_mv += Scorer::clamp( census.best(), context_ );
	set_base_motion_vector( new_mv );
      }
      break;
    case SPLITMV:
      {
	header_.partition_id.initialize( data, split_mv_probs );
	const auto & partition_scheme = mv_partitions.at( header_.partition_id.get() );

	for ( const auto & this_partition : partition_scheme ) {
	  YBlock & first_subblock = Y_.at( this_partition.front().first,
					   this_partition.front().second );
	  first_subblock.set_Y_without_Y2();

	  first_subblock.read_subblock_inter_prediction( data,
							 Scorer::clamp( census.best(), context_ ),
							 probability_tables.motion_vector_probs );

	  /* copy to rest of subblocks */

	  for ( auto it = this_partition.begin() + 1; it != this_partition.end(); it++ ) {
	    YBlock & other_subblock = Y_.at( it->first, it->second );
	    other_subblock.set_prediction_mode( first_subblock.prediction_mode() );
	    other_subblock.set_motion_vector( first_subblock.motion_vector() );
	    other_subblock.set_Y_without_Y2();
	  }
	}
      }
      break;
    default:
      throw LogicError();
    }

    /* set motion vectors of Y subblocks */
    if ( Y2_.prediction_mode() != SPLITMV ) {
      Y_.forall( [&] ( YBlock & block ) { block.set_motion_vector( base_motion_vector() ); } );
    }

    /* set motion vectors of chroma subblocks */
    U_.forall_ij( [&]( UVBlock & block, const unsigned int column, const unsigned int row ) {
	block.set_motion_vector( luma_to_chroma( Y_.at( column * 2, row * 2 ).motion_vector(),
						 Y_.at( column * 2 + 1, row * 2 ).motion_vector(),
						 Y_.at( column * 2, row * 2 + 1 ).motion_vector(),
						 Y_.at( column * 2 + 1, row * 2 + 1 ).motion_vector() ) );
      } );

    if ( continuation_ ) {
      Y_.forall( [&] ( YBlock & block ) { block.set_Y_without_Y2(); } );
    }
  }
}

InterFrameMacroblockHeader::InterFrameMacroblockHeader( BoolDecoder & data,
							const InterFrameHeader & frame_header )
  : is_inter_mb( data, frame_header.prob_inter ),
    mb_ref_frame_sel1( is_inter_mb, data, frame_header.prob_references_last ),
    mb_ref_frame_sel2( mb_ref_frame_sel1.get_or( false ), data, frame_header.prob_references_golden ),
    motion_vectors_flipped_( ( (reference() == GOLDEN_FRAME) and (frame_header.sign_bias_golden) )
			     or ( (reference() == ALTREF_FRAME) and (frame_header.sign_bias_alternate) ) )
{}

template <class FrameHeaderType, class MacroblockHeaderType>
void Macroblock<FrameHeaderType, MacroblockHeaderType>::parse_tokens( BoolDecoder & data,
								      const ProbabilityTables & probability_tables )
{
  /* is macroblock skipped? */
  if ( mb_skip_coeff_.get_or( false ) ) {
    return;
  }

  /* parse Y2 block if present */
  if ( Y2_.coded() and (not continuation_) ) {
    Y2_.parse_tokens( data, probability_tables );
    has_nonzero_ |= Y2_.has_nonzero();
  }

  /* parse Y blocks with variable first coefficient */
  Y_.forall( [&]( YBlock & block ) {
      block.parse_tokens( data, probability_tables );
      has_nonzero_ |= block.has_nonzero(); } );

  /* parse U and V blocks */
  U_.forall( [&]( UVBlock & block ) {
      block.parse_tokens( data, probability_tables );
      has_nonzero_ |= block.has_nonzero(); } );
  V_.forall( [&]( UVBlock & block ) {
      block.parse_tokens( data, probability_tables );
      has_nonzero_ |= block.has_nonzero(); } );
}

template <class FrameHeaderType, class MacroblockHeaderType>
void Macroblock<FrameHeaderType, MacroblockHeaderType>::apply_walsh( const Quantizer & quantizer,
								     VP8Raster::Macroblock & raster ) const
{
    SafeArray< SafeArray< DCTCoefficients, 4 >, 4 > Y_dequant_coeffs;
    for ( int row = 0; row < 4; row++ ) {
      for ( int column = 0; column < 4; column++ ) {
        Y_dequant_coeffs.at( row ).at( column ) = Y_.at( column, row ).dequantize( quantizer );
      }
    }
    Y2_.dequantize( quantizer ).walsh_transform( Y_dequant_coeffs );
    for ( int row = 0; row < 4; row++ ) {
      for ( int column = 0; column < 4; column++ ) {
        Y_dequant_coeffs.at( row ).at( column ).idct_add( raster.Y_sub.at( column, row ) );
      }
    }
}

template <class FrameHeaderType, class MacroblockHeaderType>
void Macroblock<FrameHeaderType, MacroblockHeaderType>::reconstruct_intra( const Quantizer & quantizer,
									   VP8Raster::Macroblock & raster ) const
{
  /* Chroma */
  raster.U.intra_predict( uv_prediction_mode() );
  raster.V.intra_predict( uv_prediction_mode() );

  if ( has_nonzero_ ) {
    U_.forall_ij( [&] ( const UVBlock & block, const unsigned int column, const unsigned int row )
		  { block.dequantize( quantizer ).idct_add( raster.U_sub.at( column, row ) ); } );
    V_.forall_ij( [&] ( const UVBlock & block, const unsigned int column, const unsigned int row )
		  { block.dequantize( quantizer ).idct_add( raster.V_sub.at( column, row ) ); } );
  }

  /* Luma */
  if ( Y2_.prediction_mode() == B_PRED ) {
    /* Prediction and inverse transform done in line! */
    Y_.forall_ij( [&] ( const YBlock & block, const unsigned int column, const unsigned int row ) {
	raster.Y_sub.at( column, row ).intra_predict( block.prediction_mode() );
	if ( has_nonzero_ ) block.dequantize( quantizer ).idct_add( raster.Y_sub.at( column, row ) ); } );
  } else {
    raster.Y.intra_predict( Y2_.prediction_mode() );

    if ( has_nonzero_ ) {
      apply_walsh( quantizer, raster );
    }
  }
}

template <>
void InterFrameMacroblock::reconstruct_continuation( const RasterHandle & continuation,
						     VP8Raster::Macroblock & raster ) const
{
  assert( continuation_ );

  const MotionVector zeromv;

  const VP8Raster & reference = continuation;

  raster.Y.inter_predict( zeromv, reference.Y() );
  raster.U.inter_predict( zeromv, reference.U() );
  raster.V.inter_predict( zeromv, reference.V() );

  assert( raster.Y.contents() == reference.macroblock( context_.column, context_.row ).Y.contents() );
  assert( raster.U.contents() == reference.macroblock( context_.column, context_.row ).U.contents() );
  assert( raster.V.contents() == reference.macroblock( context_.column, context_.row ).V.contents() );

  Y_.forall_ij( [&] ( const YBlock & block, const unsigned int column, const unsigned int row )
		{ block.add_residue( raster.Y_sub.at( column, row ) ); } );
  U_.forall_ij( [&] ( const UVBlock & block, const unsigned int column, const unsigned int row )
		{ block.add_residue( raster.U_sub.at( column, row ) ); } );
  V_.forall_ij( [&] ( const UVBlock & block, const unsigned int column, const unsigned int row )
		{ block.add_residue( raster.V_sub.at( column, row ) ); } );
}

template <>
void InterFrameMacroblock::reconstruct_inter( const Quantizer & quantizer,
					      const References & references,
					      VP8Raster::Macroblock & raster ) const
{
  assert( not continuation_ );

  const VP8Raster & reference = references.at( header_.reference() );

  if ( Y2_.prediction_mode() == SPLITMV ) {
    Y_.forall_ij( [&] ( const YBlock & block, const unsigned int column, const unsigned int row )
		  { raster.Y_sub.at( column, row ).inter_predict( block.motion_vector(),
								       reference.Y() ); } );
    U_.forall_ij( [&] ( const UVBlock & block, const unsigned int column, const unsigned int row )
		  { raster.U_sub.at( column, row ).inter_predict( block.motion_vector(),
								       reference.U() );
		    raster.V_sub.at( column, row ).inter_predict( block.motion_vector(),
								       reference.V() ); } );

    if ( has_nonzero_ ) {
      Y_.forall_ij( [&] ( const YBlock & block, const unsigned int column, const unsigned int row )
		    { block.dequantize( quantizer ).idct_add( raster.Y_sub.at( column, row ) ); } );
      U_.forall_ij( [&] ( const UVBlock & block, const unsigned int column, const unsigned int row )
		    { block.dequantize( quantizer ).idct_add( raster.U_sub.at( column, row ) ); } );
      V_.forall_ij( [&] ( const UVBlock & block, const unsigned int column, const unsigned int row )
		    { block.dequantize( quantizer ).idct_add( raster.V_sub.at( column, row ) ); } );
    }
  } else {
    raster.Y.inter_predict( base_motion_vector(), reference.Y() );
    raster.U.inter_predict( U_.at( 0, 0 ).motion_vector(), reference.U() );
    raster.V.inter_predict( U_.at( 0, 0 ).motion_vector(), reference.V() );

    if ( has_nonzero_ ) {
      apply_walsh( quantizer, raster );
      U_.forall_ij( [&] ( const UVBlock & block, const unsigned int column, const unsigned int row )
		    { block.dequantize( quantizer ).idct_add( raster.U_sub.at( column, row ) ); } );
      V_.forall_ij( [&] ( const UVBlock & block, const unsigned int column, const unsigned int row )
		    { block.dequantize( quantizer ).idct_add( raster.V_sub.at( column, row ) ); } );
    }
  }
}

template <class FrameHeaderType, class MacroblockHeaderType>
void Macroblock<FrameHeaderType, MacroblockHeaderType>::loopfilter( const Optional< FilterAdjustments > & filter_adjustments,
								    const FilterParameters & loopfilter,
								    VP8Raster::Macroblock & raster ) const
{
  const bool skip_subblock_edges = loopfilter_skip_subblock_edges_.get_or( Y2_.coded() and ( not has_nonzero_ ) );

  /* which filter are we using? */
  FilterParameters loopfilter_in_use( loopfilter );

  if ( filter_adjustments.initialized() ) {
    loopfilter_in_use.adjust( filter_adjustments.get().loopfilter_ref_adjustments,
			      filter_adjustments.get().loopfilter_mode_adjustments,
			      header_.reference(),
			      Y2_.prediction_mode() );
  }

  /* is filter disabled? */
  if ( loopfilter_in_use.filter_level <= 0 ) {
    return;
  }

  switch ( loopfilter_in_use.type ) {
  case LoopFilterType::Normal:
    {
      NormalLoopFilter filter( FrameHeaderType::key_frame(), loopfilter_in_use );
      filter.filter( raster, skip_subblock_edges );
    }
    break;
  case LoopFilterType::Simple:
    {
      SimpleLoopFilter filter( loopfilter_in_use );
      filter.filter( raster, skip_subblock_edges );
    }
    break;
  default:
    throw LogicError();
  }
}

reference_frame InterFrameMacroblockHeader::reference( void ) const
{
  if ( not is_inter_mb ) {
    return CURRENT_FRAME;
  }

  if ( not mb_ref_frame_sel1.get() ) {
    return LAST_FRAME;
  }

  if ( not mb_ref_frame_sel2.get() ) {
    return GOLDEN_FRAME;
  }

  return ALTREF_FRAME;
}

template class Macroblock< KeyFrameHeader, KeyFrameMacroblockHeader >;
template class Macroblock< InterFrameHeader, InterFrameMacroblockHeader >;
