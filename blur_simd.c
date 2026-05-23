/*
 * vim:ts=4:sw=4:expandtab
 *
 * © 2016 Sebastian Frysztak
 *
 * See LICENSE for licensing information
 *
 */

/*
 * SSE2 fast-path blur removed: the new 3-pass constant-time box blur in
 * blur.c is O(W×H) regardless of sigma and does not benefit from a
 * separate SSE2 implementation.  This file is retained because it is
 * listed in Makefile.am.
 */
#ifdef __SSE2__
#endif
