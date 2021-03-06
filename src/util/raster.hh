#ifndef RASTER_HH
#define RASTER_HH

#include <memory>
#include <boost/functional/hash.hpp>
#include <cmath>

#include "2d.hh"
#include "ssim.hh"
#include "safe_array.hh"
#include <config.h>

#include <iostream>

/* For an array of pixels, context and separate construction not necessary */
template<>
template< typename... Targs >
TwoDStorage<uint8_t>::TwoDStorage( const unsigned int width, const unsigned int height, Targs&&... Fargs )
  : width_( width ), height_( height ), storage_( width * height, Fargs... )
{
  assert( width > 0 );
  assert( height > 0 );
}

class BaseRaster
{
protected:
  unsigned int display_width_, display_height_;
  unsigned int width_, height_;

  TwoD< uint8_t > Y_ { width_, height_ },
    U_ { width_ / 2, height_ / 2 },
    V_ { width_ / 2, height_ / 2 };

  size_t raw_hash( void ) const;

public:
  BaseRaster( const unsigned int display_width, const unsigned int display_height,
    const unsigned int width, const unsigned int height );

  TwoD< uint8_t > & Y( void ) { return Y_; }
  TwoD< uint8_t > & U( void ) { return U_; }
  TwoD< uint8_t > & V( void ) { return V_; }

  const TwoD< uint8_t > & Y( void ) const { return Y_; }
  const TwoD< uint8_t > & U( void ) const { return U_; }
  const TwoD< uint8_t > & V( void ) const { return V_; }

  unsigned int width( void ) const { return width_; }
  unsigned int height( void ) const { return height_; }
  unsigned int display_width( void ) const { return display_width_; }
  unsigned int display_height( void ) const { return display_height_; }

  // SSIM as determined by libx264
  double quality( const BaseRaster & other ) const;

  bool operator==( const BaseRaster & other ) const;
  bool operator!=( const BaseRaster & other ) const;

  void copy_from( const BaseRaster & other );
  void dump( FILE * file ) const;
  void dump( int fd ) const;
};

#endif /* RASTER_HH */
