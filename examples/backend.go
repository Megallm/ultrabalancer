package main

import (
    "fmt"
    "net/http"
    "os"
)

func main() {
    if len(os.Args) < 2 {
        fmt.Println("Usage: backend <port>")
        os.Exit(1)
    }

    port := os.Args[1]
    hostname, _ := os.Hostname()

    http.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
        fmt.Fprintf(w, "Backend server on %s:%s\n", hostname, port)
        fmt.Fprintf(w, "Request from: %s\n", r.RemoteAddr)
        fmt.Fprintf(w, "X-Forwarded-For: %s\n", r.Header.Get("X-Forwarded-For"))
    })

    http.HandleFunc("/health", func(w http.ResponseWriter, r *http.Request) {
        w.WriteHeader(http.StatusOK)
        w.Write([]byte("OK"))
    })

    http.HandleFunc("/api/health", func(w http.ResponseWriter, r *http.Request) {
        w.Header().Set("Content-Type", "application/json")
        w.Write([]byte(`{"status":"ok","server":"` + hostname + `"}`))
    })

    fmt.Printf("Backend server listening on :%s\n", port)
    http.ListenAndServe(":"+port, nil)
}