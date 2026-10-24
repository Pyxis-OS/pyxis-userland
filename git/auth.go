// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

package main

import (
	"bytes"
	"context"
	"crypto/tls"
	"crypto/x509"
	"encoding/pem"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/url"
	"os"
	"strconv"
	"strings"
	"unicode"
	"unicode/utf8"

	"github.com/go-git/gcfg"
	"github.com/go-git/go-git/v5/plumbing"
	"github.com/go-git/go-git/v5/plumbing/transport"
	"github.com/go-git/go-git/v5/plumbing/transport/client"
	githttp "github.com/go-git/go-git/v5/plumbing/transport/http"
)

var (
	errCredentialConfig = errors.New("invalid home://.config/git/credentials; expected unique HTTPS origins with one username and token each")
	errHTTPSRedirect    = errors.New("refused unsafe HTTPS redirect")
	errHTTPSRedirects   = errors.New("too many HTTPS redirects")
)

// A session selects credentials once, before any request. Its URL is public
// configuration; Auth must never be printed or stored in repository config.
// The CLI performs one network operation at a time: installing this protocol
// client changes go-git's process-wide HTTPS transport.
type httpsSession struct {
	URL  string
	Auth transport.AuthMethod
}

func validateHTTPSURL(raw string) (*url.URL, error) {
	invalid := errors.New("requires a public HTTPS URL without credentials, query or fragment")
	if !utf8.ValidString(raw) || strings.ContainsFunc(raw, unicode.IsControl) {
		return nil, invalid
	}
	u, err := url.Parse(raw)
	if err != nil || u.Scheme != "https" || u.Opaque != "" || u.User != nil ||
		u.RawQuery != "" || u.ForceQuery || strings.Contains(raw, "#") ||
		strings.ContainsFunc(u.Path, unicode.IsControl) {
		return nil, invalid
	}
	if _, err := httpsOrigin(u); err != nil {
		return nil, invalid
	}
	return u, nil
}

// Only hostname case and the omitted default port are normalized. DNS
// aliases, subdomains, trailing dots and alternative IP spellings do not
// inherit another spelling's credentials.
func httpsOrigin(u *url.URL) (string, error) {
	if u == nil || u.Scheme != "https" || u.User != nil || u.Opaque != "" {
		return "", errHTTPSRedirect
	}
	host := u.Hostname()
	if host == "" || strings.ContainsAny(host, "%\\") {
		return "", errHTTPSRedirect
	}
	if strings.Contains(host, ":") {
		if net.ParseIP(host) == nil || !strings.HasPrefix(u.Host, "[") {
			return "", errHTTPSRedirect
		}
	} else {
		for _, c := range host {
			if !(c >= 'a' && c <= 'z' || c >= 'A' && c <= 'Z' ||
				c >= '0' && c <= '9' || c == '-' || c == '.') {
				return "", errHTTPSRedirect
			}
		}
	}
	port := u.Port()
	if port == "" {
		if strings.HasSuffix(u.Host, ":") {
			return "", errHTTPSRedirect
		}
		port = "443"
	} else {
		n, err := strconv.Atoi(port)
		if err != nil || n < 1 || n > 65535 || strconv.Itoa(n) != port {
			return "", errHTTPSRedirect
		}
	}
	return net.JoinHostPort(strings.ToLower(host), port), nil
}

func httpsCredentials(origin string) (transport.AuthMethod, error) {
	path, err := credentialsPath()
	if err != nil {
		return nil, errors.New("cannot locate home://.config/git/credentials")
	}
	f, err := os.Open(path)
	if errors.Is(err, os.ErrNotExist) {
		return nil, nil
	}
	if err != nil {
		return nil, errors.New("cannot read home://.config/git/credentials")
	}
	type credential struct{ username, token string }
	entries := make(map[string]*credential)
	var current *credential
	err = gcfg.ReadWithCallback(f, func(section, subsection, key, value string, blank bool) error {
		if !strings.EqualFold(section, "credential") || subsection == "" {
			return errCredentialConfig
		}
		if key == "" {
			u, err := validateHTTPSURL(subsection)
			if err != nil || u.Path != "" {
				return errCredentialConfig
			}
			selected, err := httpsOrigin(u)
			if err != nil || entries[selected] != nil {
				return errCredentialConfig
			}
			current = &credential{}
			entries[selected] = current
			return nil
		}
		if current == nil || blank || strings.TrimSpace(value) == "" ||
			!utf8.ValidString(value) || strings.ContainsFunc(value, unicode.IsControl) {
			return errCredentialConfig
		}
		switch {
		case strings.EqualFold(key, "username") && current.username == "" && !strings.Contains(value, ":"):
			current.username = value
		case strings.EqualFold(key, "token") && current.token == "":
			current.token = value
		default:
			return errCredentialConfig
		}
		return nil
	})
	closeErr := f.Close()
	// Decoder errors can quote a line containing a secret. Neither decoder
	// nor filesystem error text may escape this boundary, even when wrapped.
	if err != nil {
		return nil, errCredentialConfig
	}
	if closeErr != nil {
		return nil, errors.New("cannot close home://.config/git/credentials")
	}
	for _, entry := range entries {
		if entry.username == "" || entry.token == "" {
			return nil, errCredentialConfig
		}
	}
	entry := entries[origin]
	if entry == nil {
		return nil, nil
	}
	return &githttp.BasicAuth{Username: entry.username, Password: entry.token}, nil
}

func newHTTPS(raw string, requireCredentials bool) (*httpsSession, error) {
	u, err := validateHTTPSURL(raw)
	if err != nil {
		return nil, err
	}
	origin, err := httpsOrigin(u)
	if err != nil {
		return nil, err
	}
	auth, err := httpsCredentials(origin)
	if err != nil {
		return nil, err
	}
	if auth == nil && requireCredentials {
		return nil, errors.New("push requires matching credentials in home://.config/git/credentials")
	}
	cfg, err := readAppConfig()
	if err != nil {
		return nil, err
	}
	base, ok := http.DefaultTransport.(*http.Transport)
	if !ok {
		return nil, errors.New("HTTPS default transport unavailable")
	}
	tr := base.Clone()
	// No environment proxy or hidden proxy authentication. Leave native CA
	// discovery unchanged and require certificate and hostname verification.
	tr.Proxy = nil
	if tr.TLSClientConfig != nil {
		tr.TLSClientConfig.InsecureSkipVerify = false
	}
	var roundTripper http.RoundTripper = tr
	if ca := cfg.HTTPCA[origin]; ca != "" {
		roots, err := httpsCARoots(ca)
		if err != nil {
			return nil, err
		}
		private := tr.Clone()
		if private.TLSClientConfig == nil {
			private.TLSClientConfig = &tls.Config{}
		}
		private.TLSClientConfig.RootCAs = roots
		private.TLSClientConfig.InsecureSkipVerify = false
		roundTripper = &originCATransport{origin: origin, public: tr, private: private}
	}
	httpClient := &http.Client{Transport: roundTripper, CheckRedirect: func(request *http.Request, via []*http.Request) error {
		if len(via) >= 10 {
			return errHTTPSRedirects
		}
		next, err := httpsOrigin(request.URL)
		if err != nil || auth != nil && next != origin {
			return errHTTPSRedirect
		}
		return nil
	}}
	client.InstallProtocol("https", githttp.NewClient(httpClient))
	return &httpsSession{URL: u.String(), Auth: auth}, nil
}

// Redirects are separate RoundTrip calls. Never let the initial origin's CA
// become trusted for an anonymous redirect to another origin, even on a
// pooled connection. A different configured origin is not reselected here.
type originCATransport struct {
	origin          string
	public, private *http.Transport
}

func (tr *originCATransport) RoundTrip(request *http.Request) (*http.Response, error) {
	origin, err := httpsOrigin(request.URL)
	if err != nil {
		return nil, errHTTPSRedirect
	}
	if origin == tr.origin {
		return tr.private.RoundTrip(request)
	}
	return tr.public.RoundTrip(request)
}

func httpsCARoots(raw string) (*x509.CertPool, error) {
	path, err := caFilePath(raw)
	if err != nil {
		return nil, err
	}
	// Refuse a known nonregular input before opening it (including FIFOs on
	// the Linux reference), and verify the actual opened file again below.
	info, err := os.Lstat(path)
	if err != nil || !info.Mode().IsRegular() {
		return nil, errors.New("sslCAInfo must name a readable regular PEM certificate file")
	}
	f, err := os.Open(path)
	if err != nil {
		return nil, errors.New("cannot open sslCAInfo certificate file")
	}
	info, err = f.Stat()
	if err != nil || !info.Mode().IsRegular() {
		_ = f.Close()
		return nil, errors.New("sslCAInfo must name a readable regular PEM certificate file")
	}
	data, readErr := io.ReadAll(f)
	closeErr := f.Close()
	if readErr != nil || closeErr != nil {
		return nil, errors.New("cannot read or close sslCAInfo certificate file")
	}
	roots, err := x509.SystemCertPool()
	if err != nil || roots == nil {
		return nil, errors.New("system public CA roots unavailable for sslCAInfo")
	}
	count := 0
	for len(bytes.TrimSpace(data)) != 0 {
		data = bytes.TrimSpace(data)
		if !bytes.HasPrefix(data, []byte("-----BEGIN CERTIFICATE-----")) {
			return nil, errors.New("sslCAInfo contains malformed PEM certificate data")
		}
		const endMarker = "-----END CERTIFICATE-----"
		end := bytes.Index(data, []byte(endMarker))
		if end < 0 {
			return nil, errors.New("sslCAInfo contains malformed PEM certificate data")
		}
		end += len(endMarker)
		encoded := data[:end]
		// pem.Decode normally skips malformed blocks to find a later valid
		// one. Restrict it to this block so malformed input cannot disappear.
		block, rest := pem.Decode(encoded)
		if block == nil || len(bytes.TrimSpace(rest)) != 0 ||
			bytes.Count(encoded, []byte("-----BEGIN")) != 1 ||
			block.Type != "CERTIFICATE" || len(block.Headers) != 0 {
			return nil, errors.New("sslCAInfo contains malformed PEM certificate data")
		}
		certificate, err := x509.ParseCertificate(block.Bytes)
		if err != nil {
			return nil, errors.New("sslCAInfo contains an invalid certificate")
		}
		roots.AddCert(certificate)
		count++
		data = data[end:]
	}
	if count == 0 {
		return nil, errors.New("sslCAInfo contains no PEM certificates")
	}
	return roots, nil
}

// Server bodies, redirect locations, certificate text and nested URL/network
// errors can contain attacker-controlled text (including echoed credentials).
// Classify structured errors, then return only trusted text and the validated
// public repository URL. Never wrap or print the original transport error.
func (s *httpsSession) failure(err error) error {
	if err == nil {
		return nil
	}
	var safe error
	switch {
	case errors.Is(err, context.Canceled):
		safe = context.Canceled
	case errors.Is(err, context.DeadlineExceeded):
		safe = context.DeadlineExceeded
	case errors.Is(err, errHTTPSRedirect), errors.Is(err, errHTTPSRedirects):
		safe = errors.New("HTTPS redirect refused")
	case errors.Is(err, transport.ErrAuthenticationRequired):
		safe = errors.New("HTTP 401: authentication required")
	case errors.Is(err, transport.ErrAuthorizationFailed):
		safe = errors.New("HTTP 403: authorization failed")
	case errors.Is(err, transport.ErrRepositoryNotFound):
		safe = errors.New("HTTP 404: repository not found or inaccessible")
	case errors.Is(err, transport.ErrEmptyRemoteRepository):
		safe = transport.ErrEmptyRemoteRepository
	default:
		safe = safeHTTPSFailure(err)
	}
	return fmt.Errorf("HTTPS %s: %w", s.URL, safe)
}

func safeHTTPSFailure(err error) error {
	// The pinned library's UnexpectedError exposes Err without Unwrap.
	var unexpected *plumbing.UnexpectedError
	if errors.As(err, &unexpected) && unexpected.Err != nil {
		err = unexpected.Err
	}
	var status *githttp.Err
	if errors.As(err, &status) && status.Response != nil &&
		status.Response.StatusCode >= 100 && status.Response.StatusCode <= 599 {
		return fmt.Errorf("HTTP status %d", status.Response.StatusCode)
	}
	var verification *tls.CertificateVerificationError
	var unknown x509.UnknownAuthorityError
	var hostname x509.HostnameError
	var invalid x509.CertificateInvalidError
	var roots x509.SystemRootsError
	var record tls.RecordHeaderError
	if errors.As(err, &verification) || errors.As(err, &unknown) ||
		errors.As(err, &hostname) || errors.As(err, &invalid) ||
		errors.As(err, &roots) || errors.As(err, &record) {
		return errors.New("TLS verification or handshake failed")
	}
	var dns *net.DNSError
	if errors.As(err, &dns) {
		return errors.New("DNS lookup failed")
	}
	var network net.Error
	if errors.As(err, &network) {
		if network.Timeout() {
			return errors.New("network timeout")
		}
		return errors.New("network request failed")
	}
	return errors.New("remote transfer failed or was rejected")
}
