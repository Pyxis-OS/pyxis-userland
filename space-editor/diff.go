// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

package main

// A line diff for the save review, using Myers' O(ND) algorithm.

import "strings"

const diffLineLimit = 8192

type diffLine struct {
	op   byte // ' ', '-' or '+'
	text string
}

func splitLines(text string) []string {
	lines := strings.Split(text, "\n")
	if len(lines) > 0 && lines[len(lines)-1] == "" {
		lines = lines[:len(lines)-1]
	}
	return lines
}

func diffText(before, after string) []diffLine {
	a, b := splitLines(before), splitLines(after)
	n, m := len(a), len(b)
	// The trace holds one row per edit; very large files show whole sides.
	if n+m > diffLineLimit {
		var lines []diffLine
		for _, line := range a {
			lines = append(lines, diffLine{'-', line})
		}
		for _, line := range b {
			lines = append(lines, diffLine{'+', line})
		}
		return lines
	}
	max := n + m
	v := make([]int, 2*max+2)
	var trace [][]int
	for d := 0; d <= max; d++ {
		trace = append(trace, append([]int(nil), v...))
		for k := -d; k <= d; k += 2 {
			var x int
			if k == -d || (k != d && v[max+k-1] < v[max+k+1]) {
				x = v[max+k+1]
			} else {
				x = v[max+k-1] + 1
			}
			y := x - k
			for x < n && y < m && a[x] == b[y] {
				x++
				y++
			}
			v[max+k] = x
			if x >= n && y >= m {
				return backtrack(trace, a, b, max)
			}
		}
	}
	return nil
}

func backtrack(trace [][]int, a, b []string, max int) []diffLine {
	var lines []diffLine
	x, y := len(a), len(b)
	for d := len(trace) - 1; d > 0; d-- {
		v := trace[d]
		k := x - y
		var previous int
		if k == -d || (k != d && v[max+k-1] < v[max+k+1]) {
			previous = k + 1
		} else {
			previous = k - 1
		}
		previousX := v[max+previous]
		previousY := previousX - previous
		for x > previousX && y > previousY {
			x--
			y--
			lines = append(lines, diffLine{' ', a[x]})
		}
		if x == previousX {
			y--
			lines = append(lines, diffLine{'+', b[y]})
		} else {
			x--
			lines = append(lines, diffLine{'-', a[x]})
		}
	}
	for x > 0 && y > 0 {
		x--
		y--
		lines = append(lines, diffLine{' ', a[x]})
	}
	for i, j := 0, len(lines)-1; i < j; i, j = i+1, j-1 {
		lines[i], lines[j] = lines[j], lines[i]
	}
	return lines
}
