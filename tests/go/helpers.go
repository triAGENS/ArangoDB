//////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2021 ArangoDB GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Max Neunhoeffer
//////////////////////////////////////////////////////////////////////////////

package main

import (
	"context"
	"crypto/tls"
	"fmt"
	"net"
	"os"
	"runtime"
	"strconv"
	"strings"

	driver "github.com/arangodb/go-driver"
	driverhttp "github.com/arangodb/go-driver/http"
	"github.com/arangodb/go-driver/vst"

	"golang.org/x/net/http2"
)

const (
	ErrorNoEnvironment = 1
	ErrorNoConnection  = 2
	ErrorNoClient      = 3
)

var (
	dbCleanupList []string
	collCleanupList []string
)

func file_line() string {
	_, fileName, fileLine, ok := runtime.Caller(2)
	var s string
	if ok {
		s = fmt.Sprintf("%s:%d", fileName, fileLine)
	} else {
		s = ""
	}
	return s
}

func testGreeting(msg string) {
	fmt.Printf("==========================================================\n")
	fmt.Printf("Test starting: %s\n", msg)
	fmt.Printf("==========================================================\n\n")
}

type TestConfig struct {
	Endpoints []string
	Authentication string
	JWTSecret string
	Mode string           // can be "cluster", "resilientsingle" or "single"
	Agents []string       // endpoints of agents (for cluster/resilientsingle)
	DBServers []string    // endpoints of dbservers
	Coordinators []string // endpoints of coordinators (cluster)
}

func getFromEnv(name string) string {
	s, ok := os.LookupEnv(name)
	if !ok {
		fmt.Printf("Did not find %s!\n", name)
		os.Exit(ErrorNoEnvironment)
	}
	return s
}

func configFromEnv() TestConfig {
	return TestConfig{
		Endpoints:      strings.Split(getFromEnv("TEST_ENDPOINTS"), ","),
		Authentication: getFromEnv("TEST_AUTHENTICATION"),
		JWTSecret:      getFromEnv("TEST_JWTSECRET"),
		Mode:           getFromEnv("TEST_MODE"),
		Agents:         strings.Split(getFromEnv("TEST_AGENTS"), ","),
		DBServers:      strings.Split(getFromEnv("TEST_DBSERVERS"), ","),
		Coordinators:   strings.Split(getFromEnv("TEST_COORDINATORS"), ","),
	}
}

func makeHttp1Client(endpoints []string) driver.Client {
	conn, err := driverhttp.NewConnection(driverhttp.ConnectionConfig{
		Endpoints: endpoints,
		TLSConfig: &tls.Config{
			InsecureSkipVerify: true,
		},
	})
	if err != nil {
		fmt.Printf("Could not create client connection: %v\n", err)
		os.Exit(ErrorNoConnection)
	}
	client, err := driver.NewClient(driver.ClientConfig{
		Connection: conn,
	})
	if err != nil {
		fmt.Printf("Could not create client: %v\n", err)
		os.Exit(ErrorNoClient)
	}
	return client
}

func makeHttp2Client(endpoints []string) driver.Client {
	var conn driver.Connection
	var err error
	if len(endpoints[0]) >= 5 && endpoints[0][:5] == "https" {
		conn, err = driverhttp.NewConnection(driverhttp.ConnectionConfig{
			Endpoints: endpoints,
			Transport: &http2.Transport{
				TLSClientConfig: &tls.Config{
					InsecureSkipVerify: true,
				},
			},
		})
	} else {
		conn, err = driverhttp.NewConnection(driverhttp.ConnectionConfig{
			Endpoints: endpoints,
			Transport: &http2.Transport{
				// This is a trick for HTTP/2 without encryption (h2c):
				AllowHTTP: true,
				// Pretend we are dialing a TLS endpoint. (Note, we ignore the passed tls.Config)
				DialTLS: func(network, addr string, cfg *tls.Config) (net.Conn, error) {
					return net.Dial(network, addr)
				},
			},
		})
	}
	if err != nil {
		fmt.Printf("Could not create client connection: %v\n", err)
		os.Exit(ErrorNoConnection)
	}
	client, err := driver.NewClient(driver.ClientConfig{
		Connection: conn,
	})
	if err != nil {
		fmt.Printf("Could not create client: %v\n", err)
		os.Exit(ErrorNoClient)
	}
	return client
}

func makeVSTClient(endpoints []string) driver.Client {
	conn, err := vst.NewConnection(vst.ConnectionConfig{
		Endpoints: endpoints,
		TLSConfig: &tls.Config{
			InsecureSkipVerify: true,
		},
	})
	if err != nil {
		fmt.Printf("Could not create client connection: %v\n", err)
		os.Exit(ErrorNoConnection)
	}
	client, err := driver.NewClient(driver.ClientConfig{
		Connection: conn,
	})
	if err != nil {
		fmt.Printf("Could not create client: %v\n", err)
		os.Exit(ErrorNoClient)
	}
	return client
}

func assert(b bool, msg string) {
	if !b {
		fmt.Printf("%s: Assertion failure: %s\n", file_line(), msg)
		cleanupDatabasesAndCollections()
		os.Exit(100)
	}
}

func assertNil(err error, msg string) {
	if err != nil {
		fmt.Printf("%s: Expected no error but got: %v, %s\n", file_line(), err, msg)
		cleanupDatabasesAndCollections()
		os.Exit(101)
	}
}

type Metrics struct {
	lines []string
}

func getMetrics(client driver.Client) (Metrics, error) {
	req, err := client.Connection().NewRequest("GET", "/_admin/metrics/v2")
	if err != nil {
		return Metrics{lines: []string{}}, err
	}
	var rawResponse []byte
	ctx := driver.WithRawResponse(context.Background(), &rawResponse)
	_, _ = client.Connection().Do(ctx, req)
	// Will always return error, since the content type is not expected!
	lines := make([]string, 1200)
	prev := 0
	for i := 0; i < len(rawResponse); i++ {
		if rawResponse[i] == '\n' {
			lines = append(lines, string(rawResponse[prev:i]))
			prev = i + 1
		}
	}
	return Metrics{
		lines: lines,
	}, nil
}

func (m *Metrics) readIntMetric(metricName string) int64 {
	for i := 0; i < len(m.lines); i++ {
		if len(m.lines[i]) >= len(metricName)+1 &&
			m.lines[i][0:len(metricName)] == metricName {
			s := strings.Split(m.lines[i], " ")
			j, err := strconv.ParseInt(s[1], 10, 64)
			if err != nil {
				fmt.Printf("Metric %s : %d\n", metricName, 0)
				return 0
			}
			fmt.Printf("Metric %s : %d\n", metricName, j)
			return j
		}
	}
	return 0
}

func makeDatabase(client driver.Client, name string, opts *driver.CreateDatabaseOptions) driver.Database {
	exists, err := client.DatabaseExists(nil, name)
	assertNil(err, "Cannot check for database '" + name + "'")
	if exists {
		db, err := client.Database(nil, name)
		assertNil(err, "Cannot access database '" + name + "'")
		err = db.Remove(nil)
		assertNil(err, "Cannot drop database '" + name + "'")
	}
  db, err := client.CreateDatabase(nil, name, opts)
	assertNil(err, "Cannot create database '" + name + "'")
	dbCleanupList = append(dbCleanupList, name)
	return db
}

func makeCollection(db driver.Database, name string, opts *driver.CreateCollectionOptions) driver.Collection {
	exists, err := db.CollectionExists(nil, name)
	assertNil(err, "Cannot check for collection '" + name + "'")
	if exists {
		coll, err := db.Collection(nil, name)
		assertNil(err, "Cannot access collection '" + name + "'")
		err = coll.Remove(nil)
		assertNil(err, "Cannot drop collection '" + name + "'")
	}
  coll, err := db.CreateCollection(nil, name, opts)
	assertNil(err, "Cannot create collection '" + name + "'")
	if db.Name() == "_system" {
		collCleanupList = append(collCleanupList, name)
	}
	return coll
}

func cleanupDatabasesAndCollections() {
	config := configFromEnv()
	client := makeHttp1Client(config.Endpoints)
  for _, n := range(dbCleanupList) {
		db, err := client.Database(nil, n)
		if err == nil {
      db.Remove(nil)
		}
	}
	db, err := client.Database(nil, "_system")
	if err == nil {
		for _, n := range(collCleanupList) {
			coll, err := db.Collection(nil, n)
			if err == nil {
				coll.Remove(nil)
			}
		}
	}
}
