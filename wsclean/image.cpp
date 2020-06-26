#include "image.h"
#include "serializable.h"

#include <algorithm>
#include <cmath>

template<typename NumT>
ImageT<NumT>::ImageT(size_t width, size_t height) :
	_data(new value_type[width*height]),
	_width(width), _height(height)
{
}

template<typename NumT>
ImageT<NumT>::ImageT(size_t width, size_t height, value_type initialValue) :
	_data(new value_type[width*height]),
	_width(width), _height(height)
{
	std::fill(_data, _data+_width*_height, initialValue);
}

template<typename NumT>
ImageT<NumT>::~ImageT()
{
	delete[] _data;
}

template<typename NumT>
ImageT<NumT>::ImageT(const ImageT& source) :
	_data(new value_type[source._width * source._height]),
	_width(source._width),
	_height(source._height)
{
	std::copy(source._data, source._data + _width*_height, _data);
}

template<typename NumT>
ImageT<NumT>& ImageT<NumT>::operator=(const ImageT& source)
{
	if(_width * _height != source._width * source._height)
	{
		delete[] _data;
		_data = new value_type[source._width * source._height];
	}
	_width = source._width;
	_height = source._height;
	std::copy(source._data, source._data + _width*_height, _data);
	return *this;
}

template<typename NumT>
ImageT<NumT>& ImageT<NumT>::operator=(value_type value)
{
	for(value_type& v : *this)
		v = value;
	return *this;
}

template<typename NumT>
ImageT<NumT>::ImageT(ImageT&& source) :
	_data(source._data),
	_width(source._width),
	_height(source._height)
{
	source._width = 0;
	source._height = 0;
	source._data = nullptr;
}

template<typename NumT>
ImageT<NumT>& ImageT<NumT>::operator=(ImageT&& source)
{
	std::swap(_data, source._data);
	std::swap(_width, source._width);
	std::swap(_height, source._height);
	return *this;
}

template<typename NumT>
void ImageT<NumT>::reset()
{
	delete[] _data;
	_data = nullptr;
	_width = 0;
	_height = 0;
}

template<>
void ImageT<double>::Serialize ( std::ostream& stream ) const
{
	Serializable::SerializeToUInt64(stream, _width);
	Serializable::SerializeToUInt64(stream, _height);
	for(size_t i=0; i!=_width*_height; ++i)
		Serializable::SerializeToDouble(stream, _data[i]);
}

template<>
void ImageT<double>::Unserialize ( std::istream& stream )
{
	delete[] _data;
	_width = Serializable::UnserializeUInt64(stream);
	_height = Serializable::UnserializeUInt64(stream);
	if(_width * _height == 0)
		_data = nullptr;
	else
		_data = new value_type[_width*_height];
	for(size_t i=0; i!=_width*_height; ++i)
		_data[i] = Serializable::UnserializeDouble(stream);
}

template<>
void ImageT<float>::Serialize ( std::ostream& stream ) const
{
	Serializable::SerializeToUInt64(stream, _width);
	Serializable::SerializeToUInt64(stream, _height);
	for(size_t i=0; i!=_width*_height; ++i)
		Serializable::SerializeToFloat(stream, _data[i]);
}

template<>
void ImageT<float>::Unserialize ( std::istream& stream )
{
	delete[] _data;
	_width = Serializable::UnserializeUInt64(stream);
	_height = Serializable::UnserializeUInt64(stream);
	if(_width * _height == 0)
		_data = nullptr;
	else
		_data = new value_type[_width*_height];
	for(size_t i=0; i!=_width*_height; ++i)
		_data[i] = Serializable::UnserializeFloat(stream);
}

template<typename NumT>
ImageT<NumT>& ImageT<NumT>::operator+=(const ImageT& other)
{
	for(size_t i=0; i!=_width*_height; ++i)
		_data[i] += other[i];
	return *this;
}

template<typename NumT>
ImageT<NumT>& ImageT<NumT>::operator-=(const ImageT& other)
{
	for(size_t i=0; i!=_width*_height; ++i)
		_data[i] -= other[i];
	return *this;
}

template<typename NumT>
ImageT<NumT>& ImageT<NumT>::operator*=(value_type factor)
{
	for(size_t i=0; i!=_width*_height; ++i)
		_data[i] *= factor;
	return *this;
}

template<typename NumT>
ImageT<NumT>& ImageT<NumT>::operator*=(const ImageT& other)
{
	for(size_t i=0; i!=_width*_height; ++i)
		_data[i] *= other[i];
	return *this;
}

// Cut-off the borders of an image.
// @param outWidth Should be <= inWidth.
// @param outHeight Should be <= inHeight.
template<typename NumT>
void ImageT<NumT>::Trim(value_type* output, size_t outWidth, size_t outHeight, const value_type* input, size_t inWidth, size_t inHeight)
{
	size_t startX = (inWidth - outWidth) / 2;
	size_t startY = (inHeight - outHeight) / 2;
	size_t endY = (inHeight + outHeight) / 2;
	for(size_t y=startY; y!=endY; ++y)
	{
		memcpy(&output[(y-startY)*outWidth], &input[y*inWidth + startX], outWidth*sizeof(value_type));
	}
}

/** Extend an image with zeros, complement of Trim.
	* @param outWidth Should be &gt;= inWidth.
	* @param outHeight Should be &gt;= inHeight.
	*/
template<typename NumT>
void ImageT<NumT>::Untrim(value_type* output, size_t outWidth, size_t outHeight, const value_type* input, size_t inWidth, size_t inHeight)
{
	size_t startX = (outWidth - inWidth) / 2;
	size_t endX = (outWidth + inWidth) / 2;
	size_t startY = (outHeight - inHeight) / 2;
	size_t endY = (outHeight + inHeight) / 2;
	for(size_t y=0; y!=startY; ++y)
	{
		value_type* ptr = &output[y*outWidth];
		for(size_t x=0; x!=outWidth; ++x)
			ptr[x] = 0.0;
	}
	for(size_t y=startY; y!=endY; ++y)
	{
		value_type* ptr = &output[y*outWidth];
		for(size_t x=0; x!=startX; ++x)
			ptr[x] = 0.0;
		memcpy(&output[y*outWidth + startX], &input[(y-startY)*inWidth], inWidth*sizeof(value_type));
		for(size_t x=endX; x!=outWidth; ++x)
			ptr[x] = 0.0;
	}
	for(size_t y=endY; y!=outHeight; ++y)
	{
		value_type* ptr = &output[y*outWidth];
		for(size_t x=0; x!=outWidth; ++x)
			ptr[x] = 0.0;
	}
}

template<typename NumT>
typename ImageT<NumT>::value_type ImageT<NumT>::Sum() const
{
	value_type sum = 0.0;
	for(const value_type& v : *this)
		sum += v;
	return sum;
}

template<typename NumT>
typename ImageT<NumT>::value_type ImageT<NumT>::Average() const
{
	return Sum() / NumT(size());
}

template<>
typename ImageT<double>::value_type ImageT<double>::Min() const
{
	return *std::min_element(begin(), end());
}

template<>
typename ImageT<double>::value_type ImageT<double>::Max() const
{
	return *std::max_element(begin(), end());
}

template<>
typename ImageT<double>::value_type ImageT<double>::median_with_copy(const value_type* data, size_t size, ao::uvector<value_type>& copy)
{
	copy.reserve(size);
	for(const value_type* i=data ; i!=data+size; ++i)
	{
		if(std::isfinite(*i))
			copy.push_back(*i);
	}
	if(copy.empty())
		return 0.0;
	else {
		bool even = (copy.size()%2) == 0;
		typename ao::uvector<value_type>::iterator mid = copy.begin()+(copy.size()-1)/2;
		std::nth_element(copy.begin(), mid, copy.end());
		value_type median = *mid;
		if(even)
		{
			std::nth_element(mid, mid+1, copy.end());
			median = (median + *(mid+1)) * 0.5;
		}
		return median;
	}
}

template<typename NumT>
typename ImageT<NumT>::value_type ImageT<NumT>::median_with_copy(const value_type*, size_t, ao::uvector<value_type>&)
{ 
	throw std::runtime_error("not implemented");
}

template<>
typename ImageT<double>::value_type ImageT<double>::MAD(const value_type* data, size_t size)
{
	ao::uvector<value_type> copy;
	value_type median = median_with_copy(data, size, copy);
	if(copy.empty())
		return 0.0;
		
	// Replace all values by the difference from the mean
	typename ao::uvector<value_type>::iterator mid = copy.begin()+(copy.size()-1)/2;
	for(typename ao::uvector<value_type>::iterator i=copy.begin(); i!=mid+1; ++i)
		*i = median - *i;
	for(typename ao::uvector<value_type>::iterator i=mid+1; i!=copy.end(); ++i)
		*i = *i - median;
	
	std::nth_element(copy.begin(), mid, copy.end());
	median = *mid;
	bool even = (copy.size()%2) == 0;
	if(even)
	{
		std::nth_element(mid, mid+1, copy.end());
		median = (median + *(mid+1)) * 0.5;
	}
	return median;
}

template<typename NumT>
typename ImageT<NumT>::value_type ImageT<NumT>::MAD(const value_type*, size_t)
{ 
	throw std::runtime_error("not implemented");
}


template class ImageT<double>;
template class ImageT<float>;
template class ImageT<std::complex<double>>;
template class ImageT<std::complex<float>>;
