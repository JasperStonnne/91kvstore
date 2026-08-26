


package main

import (
	"fmt"
	"net"
	"os"
)

func main() {
	conn, err := net.Dial("tcp", "172.16.145.132:2000")
	if err != nil {
		fmt.Println("connect failed: ", err)
		os.Exit(1)
	}
	defer conn.Close()

	message := "SET Teacher King"
	_, err = conn.Write([]byte(message))
	if err != nil {
		fmt.Println("send failed: ", err)
		os.Exit(1)
	}

	fmt.Printf("send msg: %s\n", message)

	buffer := make([]byte, 1024)
	length, err := conn.Read(buffer)
	if err != nil {
		fmt.Println("recv failed: ", err)
		os.Exit(1)
	}

	response := string(buffer[:length])
	fmt.Printf("recv msg: %s\n", response)
}




