#include <algorithm>
#include <limits>
#include <random>
#include "common/zassert.h"
#include "graph/image_filter.h"
#include "depth/quantize.h"

#include "gtest/gtest.h"
#include "audit_buffer.h"

namespace {

template <class InputIt, class T>
bool contains_only(InputIt first, InputIt last, const T &value)
{
	return std::find_if(first, last, [=](const T &x) { return x != value; }) == last;
}

template <class T>
T float_as(float x)
{
	zassert(false, "fail");
	return{};
}

template <>
uint16_t float_as<uint16_t>(float x)
{
	return zimg::depth::float_to_half(x);
}

template <>
float float_as<float>(float x)
{
	return x;
}

template <class T>
class Mt19937Generator {
	std::mt19937 m_gen;
	uint32_t m_depth;
	bool m_float;
	bool m_chroma;
public:
	Mt19937Generator(unsigned p, unsigned i, unsigned left, const zimg::PixelFormat &format, bool chroma) :
		m_gen{ (static_cast<uint_fast32_t>(p) << 30) | i },
		m_depth{ static_cast<uint32_t>(format.depth) },
		m_float{ format.type == zimg::PixelType::HALF || format.type == zimg::PixelType::FLOAT },
		m_chroma{ chroma }
	{
		m_gen.discard(left);
	}

	T operator()()
	{
		if (m_float) {
			double x = static_cast<double>(m_gen() - std::mt19937::min()) / static_cast<double>(std::mt19937::max() - std::mt19937::min());
			float xf = static_cast<float>(x);
			return float_as<T>(m_chroma ? xf - 0.5f : xf);
		} else {
			return static_cast<T>(m_gen() & ((1 << m_depth) - 1));
		}
	}
};

} // namespace


template <class T>
T AuditBuffer<T>::splat_byte(unsigned char b)
{
	T val;
	std::fill_n(reinterpret_cast<unsigned char *>(&val), sizeof(T), b);
	return val;
}

template <class T>
void AuditBuffer<T>::add_guard_bytes()
{
	for (unsigned p = 0; p < planes(); ++p) {
		std::fill(m_vector[p].begin(), m_vector[p].begin() + m_buffer[p].stride() / static_cast<ptrdiff_t>(sizeof(T)), m_guard_val);

		for (unsigned i = 0; i < m_buffer_height[p]; ++i) {
			T *line_base = m_buffer[p][i];

			T *line_guard_left = line_base - zimg::AlignmentOf<T>::value;
			T *line_guard_right = line_guard_left + stride_T(p);

			std::fill(line_guard_left, line_base, m_guard_val);
			std::fill(line_base + m_width[p], line_guard_right, m_guard_val);
		}

		std::fill(m_vector[p].end() - stride_T(p), m_vector[p].end(), m_guard_val);
	}
}

template <class T>
ptrdiff_t AuditBuffer<T>::stride_T(unsigned p) const
{
	return m_buffer[p].stride() / static_cast<ptrdiff_t>(sizeof(T));
}

template <class T>
unsigned AuditBuffer<T>::planes() const
{
	return m_buffer_type == AuditBufferType::PLANE ? 1 : 3;
}

template <class T>
AuditBuffer<T>::AuditBuffer(AuditBufferType buffer_type, unsigned width, unsigned height, const zimg::PixelFormat &format,
							unsigned lines, unsigned subsample_w, unsigned subsample_h) :
	m_buffer_type{ buffer_type },
	m_format(format),
	m_width{},
	m_buffer_height{},
	m_subsample_w{ subsample_w },
	m_subsample_h{ subsample_h },
	m_fill_val{ splat_byte(0xCD), splat_byte(0xCD), splat_byte(0xCD) },
	m_guard_val{ splat_byte(0xFE) }
{
	unsigned mask = zimg::graph::select_zimg_buffer_mask(lines);

	for (unsigned p = 0; p < planes(); ++p) {
		unsigned width_plane = p ? width >> subsample_w : width;
		unsigned height_plane = p ? height >> subsample_h : height;

		unsigned mask_plane = p ? (mask == zimg::graph::BUFFER_MAX ? mask : mask >> subsample_h) : mask;
		unsigned buffer_height = (mask_plane == zimg::graph::BUFFER_MAX) ? height_plane : mask_plane + 1;

		size_t guarded_linesize = (zimg::ceil_n(width_plane, zimg::AlignmentOf<T>::value) + 2 * zimg::AlignmentOf<T>::value) * sizeof(T);
		size_t guarded_linecount = buffer_height + 2;

		m_vector[p].resize(guarded_linesize * guarded_linecount / sizeof(T));
		m_buffer[p] = zimg::graph::ImageBuffer<T>{
			m_vector[p].data() + guarded_linesize / sizeof(T) + zimg::AlignmentOf<T>::value,
			static_cast<ptrdiff_t>(guarded_linesize),
			mask_plane
		};

		m_width[p] = width_plane;
		m_buffer_height[p] = buffer_height;
	}

	add_guard_bytes();
}

template <class T>
void AuditBuffer<T>::set_fill_val(unsigned char x)
{
	set_fill_val(x, 0);
	set_fill_val(x, 1);
	set_fill_val(x, 2);
}

template <class T>
void AuditBuffer<T>::set_fill_val(unsigned char x, unsigned plane)
{
	m_fill_val[plane] = splat_byte(x);
}

template <class T>
bool AuditBuffer<T>::detect_write(unsigned i, unsigned left, unsigned right) const
{
	bool write = true;

	for (unsigned p = 0; p < planes(); ++p) {
		unsigned i_plane = i >> (p ? m_subsample_h : 0);
		unsigned left_plane = left >> (p ? m_subsample_w : 0);
		unsigned right_plane = right >> (p ? m_subsample_h : 0);

		write = write && !contains_only(m_buffer[p][i_plane] + left_plane, m_buffer[p][i_plane] + right_plane, m_fill_val[p]);
	}
	return write;
}

template <class T>
void AuditBuffer<T>::assert_eq(const AuditBuffer &other, unsigned i, unsigned left, unsigned right) const
{
	for (unsigned p = 0; p < planes(); ++p) {
		unsigned left_plane = left >> (p ? m_subsample_w : 0);
		unsigned right_plane = right >> (p ? m_subsample_h : 0);

		if (std::equal(m_buffer[p][i] + left_plane, m_buffer[p][i] + right_plane, other.m_buffer[p][i] + left_plane))
			continue;

		for (unsigned j = left_plane; j < right_plane; ++j) {
			ASSERT_EQ(other.m_buffer[p][i][j], m_buffer[p][i][j]) << "mismatch at plane " << p << " (" << i << ", " << j << ")";
		}
	}
}

template <class T>
void AuditBuffer<T>::assert_guard_bytes() const
{
	for (unsigned p = 0; p < planes(); ++p) {
		ASSERT_TRUE(contains_only(m_vector[p].begin(), m_vector[p].begin() + stride_T(p), m_guard_val)) <<
			"header guard bytes corrupted";

		for (unsigned i = 0; i < m_buffer_height[p]; ++i) {
			const T *line_base = m_buffer[p].data() + static_cast<ptrdiff_t>(i) * stride_T(p);
			const T *line_guard_left = line_base - zimg::AlignmentOf<T>::value;
			const T *line_guard_right = line_guard_left + stride_T(p);

			ASSERT_TRUE(contains_only(line_guard_left, line_base, m_guard_val)) <<
				"line guard header corrupted at: " << i;
			ASSERT_TRUE(contains_only(line_base + m_width[p], line_guard_right, m_guard_val)) <<
				"line guard footer corrupted at: " << i;
		}

		ASSERT_TRUE(contains_only(m_vector[p].end() - stride_T(p), m_vector[p].end(), m_guard_val)) <<
			"footer guard bytes corrupted";
	}
}

template <class T>
void AuditBuffer<T>::random_fill(unsigned first_row, unsigned last_row, unsigned first_col, unsigned last_col)
{
	for (unsigned p = 0; p < planes(); ++p) {
		unsigned first_row_plane = first_row << (p ? m_subsample_h : 0);
		unsigned last_row_plane = last_row << (p ? m_subsample_h : 0);
		unsigned first_col_plane = first_col << (p ? m_subsample_w : 0);
		unsigned last_col_plane = last_col << (p ? m_subsample_w : 0);

		bool chroma = (m_buffer_type == AuditBufferType::PLANE && m_format.chroma) ||
			(m_buffer_type == AuditBufferType::COLOR_RGB && p > 0);

		for (unsigned i = first_row_plane; i < last_row_plane; ++i) {
			Mt19937Generator<T> engine{ p, i, first_col_plane, m_format, chroma };

			std::generate(m_buffer[p][i] + first_col_plane, m_buffer[p][i] + last_col_plane, engine);
		}
	}
}

template <class T>
void AuditBuffer<T>::default_fill()
{
	for (unsigned p = 0; p < planes(); ++p) {
		for (unsigned i = 0; i < m_buffer_height[p]; ++i) {
			std::fill_n(m_buffer[p][i], m_width[p], m_fill_val[p]);
		}
	}
}

template <class T>
zimg::graph::ColorImageBuffer<const void> AuditBuffer<T>::as_read_buffer() const
{
	return const_cast<AuditBuffer *>(this)->as_write_buffer();
}

template <class T>
zimg::graph::ColorImageBuffer<void> AuditBuffer<T>::as_write_buffer() const
{
	return m_buffer;
}

template class AuditBuffer<uint8_t>;
template class AuditBuffer<uint16_t>;
template class AuditBuffer<float>;
