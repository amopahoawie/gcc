// Safe sequence/iterator base implementation  -*- C++ -*-

// Copyright (C) 2003-2025 Free Software Foundation, Inc.
//
// This file is part of the GNU ISO C++ Library.  This library is free
// software; you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the
// Free Software Foundation; either version 3, or (at your option)
// any later version.

// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

// Under Section 7 of GPL version 3, you are granted additional
// permissions described in the GCC Runtime Library Exception, version
// 3.1, as published by the Free Software Foundation.

// You should have received a copy of the GNU General Public License and
// a copy of the GCC Runtime Library Exception along with this program;
// see the files COPYING3 and COPYING.RUNTIME respectively.  If not, see
// <http://www.gnu.org/licenses/>.

/** @file debug/safe_base.h
 *  This file is a GNU debug extension to the Standard C++ Library.
 */

#ifndef _GLIBCXX_DEBUG_SAFE_BASE_H
#define _GLIBCXX_DEBUG_SAFE_BASE_H 1

#include <ext/concurrence.h>

namespace __gnu_debug
{
  class _Safe_sequence_base;

  /** \brief Basic functionality for a @a safe iterator.
   *
   *  The %_Safe_iterator_base base class implements the functionality
   *  of a safe iterator that is not specific to a particular iterator
   *  type. It contains a pointer back to the sequence it references
   *  along with iterator version information and pointers to form a
   *  doubly-linked list of iterators referenced by the container.
   *
   *  This class must not perform any operations that can throw an
   *  exception, or the exception guarantees of derived iterators will
   *  be broken.
   */
  class _Safe_iterator_base
  {
    friend class _Safe_sequence_base;

  public:
    /** The sequence this iterator references; may be NULL to indicate
     *  a singular iterator. Stored as pointer-to-const because sequence
     *  could be declared as const.
     */
    const _Safe_sequence_base*	_M_sequence;

    /** The version number of this iterator. The sentinel value 0 is
     *  used to indicate an invalidated iterator (i.e., one that is
     *  singular because of an operation on the container). This
     *  version number must equal the version number in the sequence
     *  referenced by _M_sequence for the iterator to be
     *  non-singular.
     */
    unsigned int		_M_version;

    /** Pointer to the previous iterator in the sequence's list of
	iterators. Only valid when _M_sequence != NULL. */
    _Safe_iterator_base*	_M_prior;

    /** Pointer to the next iterator in the sequence's list of
	iterators. Only valid when _M_sequence != NULL. */
    _Safe_iterator_base*	_M_next;

  protected:
    /** Initializes the iterator and makes it singular. */
    _GLIBCXX20_CONSTEXPR
    _Safe_iterator_base()
    : _M_sequence(0), _M_version(0), _M_prior(0), _M_next(0)
    { }

    /** Initialize the iterator to reference the sequence pointed to
     *  by @p __seq. @p __constant is true when we are initializing a
     *  constant iterator, and false if it is a mutable iterator. Note
     *  that @p __seq may be NULL, in which case the iterator will be
     *  singular. Otherwise, the iterator will reference @p __seq and
     *  be nonsingular.
     */
    _GLIBCXX20_CONSTEXPR
    _Safe_iterator_base(const _Safe_sequence_base* __seq, bool __constant)
    : _M_sequence(0), _M_version(0), _M_prior(0), _M_next(0)
    {
      if (!std::__is_constant_evaluated())
	this->_M_attach(__seq, __constant);
    }

    /** Initializes the iterator to reference the same sequence that
	@p __x does. @p __constant is true if this is a constant
	iterator, and false if it is mutable. */
    _GLIBCXX20_CONSTEXPR
    _Safe_iterator_base(const _Safe_iterator_base& __x, bool __constant)
    : _M_sequence(0), _M_version(0), _M_prior(0), _M_next(0)
    {
      if (!std::__is_constant_evaluated())
	this->_M_attach(__x._M_sequence, __constant);
    }

    _GLIBCXX20_CONSTEXPR
    ~_Safe_iterator_base()
    {
      if (!std::__is_constant_evaluated())
	this->_M_detach();
    }

    /** For use in _Safe_iterator. */
    __gnu_cxx::__mutex&
    _M_get_mutex() _GLIBCXX_USE_NOEXCEPT;

    /** Attaches this iterator to the given sequence, detaching it
     *	from whatever sequence it was attached to originally. If the
     *	new sequence is the NULL pointer, the iterator is left
     *	unattached.
     */
    void
    _M_attach(const _Safe_sequence_base* __seq, bool __constant);

    /** Likewise, but not thread-safe. */
    void
    _M_attach_single(const _Safe_sequence_base* __seq,
		     bool __constant) _GLIBCXX_USE_NOEXCEPT;

    /** Detach the iterator for whatever sequence it is attached to,
     *	if any.
    */
    void
    _M_detach();

#if !_GLIBCXX_INLINE_VERSION
  private:
    /***************************************************************/
    /** Not-const method preserved for abi backward compatibility. */
    void
    _M_attach(_Safe_sequence_base* __seq, bool __constant);

    void
    _M_attach_single(_Safe_sequence_base* __seq,
		     bool __constant) _GLIBCXX_USE_NOEXCEPT;
    /***************************************************************/
#endif

  public:
    /** Likewise, but not thread-safe. */
    void
    _M_detach_single() _GLIBCXX_USE_NOEXCEPT;

    /** Determines if we are attached to the given sequence. */
    bool
    _M_attached_to(const _Safe_sequence_base* __seq) const
    { return _M_sequence == __seq; }

    /** Is this iterator singular? */
    _GLIBCXX_PURE bool
    _M_singular() const _GLIBCXX_USE_NOEXCEPT;

    /** Can we compare this iterator to the given iterator @p __x?
	Returns true if both iterators are nonsingular and reference
	the same sequence. */
    _GLIBCXX_PURE bool
    _M_can_compare(const _Safe_iterator_base& __x) const _GLIBCXX_USE_NOEXCEPT;

    /** Invalidate the iterator, making it singular. */
    void
    _M_invalidate()
    { _M_version = 0; }

    /** Reset all member variables */
    void
    _M_reset() _GLIBCXX_USE_NOEXCEPT;

    /** Unlink itself */
    void
    _M_unlink() _GLIBCXX_USE_NOEXCEPT
    {
      if (_M_prior)
	_M_prior->_M_next = _M_next;
      if (_M_next)
	_M_next->_M_prior = _M_prior;
    }
  };

  /** Iterators that derive from _Safe_iterator_base can be determined singular
   *  or non-singular.
   **/
  inline bool
  __check_singular_aux(const _Safe_iterator_base* __x)
  { return __x->_M_singular(); }

  /**
   * @brief Base class that supports tracking of iterators that
   * reference a sequence.
   *
   * The %_Safe_sequence_base class provides basic support for
   * tracking iterators into a sequence. Sequences that track
   * iterators must derived from %_Safe_sequence_base publicly, so
   * that safe iterators (which inherit _Safe_iterator_base) can
   * attach to them. This class contains two linked lists of
   * iterators, one for constant iterators and one for mutable
   * iterators, and a version number that allows very fast
   * invalidation of all iterators that reference the container.
   *
   * This class must ensure that no operation on it may throw an
   * exception, otherwise @a safe sequences may fail to provide the
   * exception-safety guarantees required by the C++ standard.
   */
  class _Safe_sequence_base
  {
    friend class _Safe_iterator_base;

  public:
    /// The list of mutable iterators that reference this container
    mutable _Safe_iterator_base* _M_iterators;

    /// The list of constant iterators that reference this container
    mutable _Safe_iterator_base* _M_const_iterators;

    /// The container version number. This number may never be 0.
    mutable unsigned int _M_version;

  protected:
    // Initialize with a version number of 1 and no iterators
    _GLIBCXX20_CONSTEXPR
    _Safe_sequence_base() _GLIBCXX_NOEXCEPT
    : _M_iterators(0), _M_const_iterators(0), _M_version(1)
    { }

#if __cplusplus >= 201103L
    _GLIBCXX20_CONSTEXPR
    _Safe_sequence_base(const _Safe_sequence_base&) noexcept
    : _Safe_sequence_base() { }

    // Move constructor swap iterators.
    _GLIBCXX20_CONSTEXPR
    _Safe_sequence_base(_Safe_sequence_base&& __seq) noexcept
    : _Safe_sequence_base()
    {
      if (!std::__is_constant_evaluated())
	_M_swap(__seq);
    }
#endif

    /** Notify all iterators that reference this sequence that the
	sequence is being destroyed. */
    _GLIBCXX20_CONSTEXPR
    ~_Safe_sequence_base()
    {
      if (!std::__is_constant_evaluated())
	this->_M_detach_all();
    }

    /** Detach all iterators, leaving them singular. */
    void
    _M_detach_all() const;

    /** Detach all singular iterators.
     *  @post for all iterators i attached to this sequence,
     *   i->_M_version == _M_version.
     */
    void
    _M_detach_singular() const;

    /** Revalidates all attached singular iterators.  This method may
     *  be used to validate iterators that were invalidated before
     *  (but for some reason, such as an exception, need to become
     *  valid again).
     */
    void
    _M_revalidate_singular() const;

    /** Swap this sequence with the given sequence. This operation
     *  also swaps ownership of the iterators, so that when the
     *  operation is complete all iterators that originally referenced
     *  one container now reference the other container.
     */
    void
    _M_swap(const _Safe_sequence_base& __x) const _GLIBCXX_USE_NOEXCEPT;

    /** For use in _Safe_sequence. */
    __gnu_cxx::__mutex&
    _M_get_mutex() const _GLIBCXX_USE_NOEXCEPT;

    /** Invalidates all iterators. */
    void
    _M_invalidate_all() const
    { if (++_M_version == 0) _M_version = 1; }

  private:
#if !_GLIBCXX_INLINE_VERSION
    /***************************************************************/
    /** Not-const method preserved for abi backward compatibility. */
    void
    _M_detach_all();

    void
    _M_detach_singular();

    void
    _M_revalidate_singular();

    void
    _M_swap(_Safe_sequence_base& __x) _GLIBCXX_USE_NOEXCEPT;

    __gnu_cxx::__mutex&
    _M_get_mutex() _GLIBCXX_USE_NOEXCEPT;
    /***************************************************************/
#endif

    /** Attach an iterator to this sequence. */
    void
    _M_attach(_Safe_iterator_base* __it, bool __constant) const;

    /** Likewise but not thread safe. */
    void
    _M_attach_single(_Safe_iterator_base* __it,
		     bool __constant) const _GLIBCXX_USE_NOEXCEPT;

    /** Detach an iterator from this sequence */
    void
    _M_detach(_Safe_iterator_base* __it) const;

    /** Likewise but not thread safe. */
    void
    _M_detach_single(_Safe_iterator_base* __it) const _GLIBCXX_USE_NOEXCEPT;
  };
} // namespace __gnu_debug

#endif
