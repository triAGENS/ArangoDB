package main

import (
	"fmt"
)

func main() {
	testGreeting("Protocol specific metrics move")
	config := configFromEnv()

	client := makeHttp1Client(config)
	versionInfo, err := client.Version(nil)
	assertNil(err, "could not get version")
	fmt.Printf("Version found: %s\n", versionInfo.Version)
}
