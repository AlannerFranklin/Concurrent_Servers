package main

import (
	"bufio"
	"flag"
	"fmt"
	"io"
	"net"
	"os"
	"sort"
	"sync"
	"sync/atomic"
	"time"
)

// 配置参数
var (
	targetAddr  = flag.String("addr", "localhost:9090", "Target server address")
	concurrency = flag.Int("c", 100, "Number of concurrent connections")
	duration    = flag.Duration("d", 10*time.Second, "Test duration")
	msgSize     = flag.Int("s", 64, "Payload size in bytes")
	saveResults = flag.Bool("save", false, "Save results to benchmark_results.csv")
	serverName  = flag.String("name", "Unknown", "Server name for the report (e.g. 'Threaded Server')")
)

// 统计指标
var (
	totalReqs   int64
	totalErrors int64
	latencies   []time.Duration
	latMu       sync.Mutex
)

func main() {
	flag.Parse()

	fmt.Printf("🔥 Starting benchmark against %s\n", *targetAddr)
	fmt.Printf("   Concurrency: %d connections\n", *concurrency)
	fmt.Printf("   Duration:    %v\n", *duration)
	fmt.Printf("   Payload:     %d bytes\n", *msgSize)
	fmt.Println("--------------------------------------------------")

	var wg sync.WaitGroup
	start := time.Now()

	// 启动并发客户端
	for i := 0; i < *concurrency; i++ {
		wg.Add(1)
		go func(id int) {
			defer wg.Done()
			runClient(id)
		}(i)
	}

	wg.Wait()
	elapsed := time.Since(start)

	printReport(elapsed)
}

func runClient(id int) {
	conn, err := net.DialTimeout("tcp", *targetAddr, 5*time.Second)
	if err != nil {
		atomic.AddInt64(&totalErrors, 1)
		// fmt.Printf("Client %d connect error: %v\n", id, err)
		return
	}
	defer conn.Close()

	reader := bufio.NewReader(conn)
	
	// 构造测试数据: ^ + payload + $
	// 例如: ^AAAA$
	payload := make([]byte, *msgSize)
	for i := range payload {
		payload[i] = 'a' // 使用小写字母，期望服务器返回大写 B
	}
	reqMsg := append([]byte{'^'}, payload...)
	reqMsg = append(reqMsg, '$')

	// 期望的响应长度 = payload 长度
	expectedReplyLen := *msgSize
	replyBuf := make([]byte, expectedReplyLen)

	endTime := time.Now().Add(*duration)

	// 初始握手: 读取服务端发送的 '*'
	// 注意：有些服务器实现可能没有发送 '*'，或者协议有变。
	// 这里我们假设标准实现会发送 '*'。
	// 如果连接后没有读到 '*'，可能是服务器实现差异，这里做一个带超时的读取。
	conn.SetReadDeadline(time.Now().Add(2 * time.Second))
	handshakeByte, err := reader.ReadByte()
	if err != nil {
		// 可能是服务器没发握手包，或者连接超时
		// fmt.Printf("Client %d handshake error: %v\n", id, err)
		atomic.AddInt64(&totalErrors, 1)
		return
	}
	if handshakeByte != '*' {
		// fmt.Printf("Client %d unexpected handshake: %c\n", id, handshakeByte)
		// atomic.AddInt64(&totalErrors, 1)
		// return
		// 如果不是 *，可能服务器直接进入状态了，我们尝试继续
	}
	conn.SetReadDeadline(time.Time{}) // 清除超时

	localLats := make([]time.Duration, 0, 1000)

	for time.Now().Before(endTime) {
		reqStart := time.Now()

		// 发送请求
		_, err := conn.Write(reqMsg)
		if err != nil {
			atomic.AddInt64(&totalErrors, 1)
			break
		}

		// 接收响应
		_, err = io.ReadFull(reader, replyBuf)
		if err != nil {
			atomic.AddInt64(&totalErrors, 1)
			break
		}

		lat := time.Since(reqStart)
		localLats = append(localLats, lat)
		atomic.AddInt64(&totalReqs, 1)
	}

	// 汇总延迟数据
	latMu.Lock()
	latencies = append(latencies, localLats...)
	latMu.Unlock()
}

func printReport(elapsed time.Duration) {
	reqs := atomic.LoadInt64(&totalReqs)
	errs := atomic.LoadInt64(&totalErrors)
	
	if reqs == 0 {
		fmt.Println("\n❌ No requests completed successfully.")
		fmt.Printf("   Total Errors: %d\n", errs)
		return
	}

	qps := float64(reqs) / elapsed.Seconds()

	// 计算延迟统计
	latMu.Lock()
	sortedLats := make([]time.Duration, len(latencies))
	copy(sortedLats, latencies)
	latMu.Unlock()
	
	sort.Slice(sortedLats, func(i, j int) bool {
		return sortedLats[i] < sortedLats[j]
	})

	p50 := sortedLats[len(sortedLats)*50/100]
	p99 := sortedLats[len(sortedLats)*99/100]
	maxLat := sortedLats[len(sortedLats)-1]
	
	// 计算平均值
	var totalLat time.Duration
	for _, l := range sortedLats {
		totalLat += l
	}
	avgLat := time.Duration(int64(totalLat) / int64(len(sortedLats)))

	fmt.Println("\n📊 Benchmark Results:")
	fmt.Printf("   Time Taken:    %.2fs\n", elapsed.Seconds())
	fmt.Printf("   Total Reqs:    %d\n", reqs)
	fmt.Printf("   Total Errors:  %d\n", errs)
	fmt.Printf("   QPS:           %.2f req/sec\n", qps)
	fmt.Println("--------------------------------------------------")
	fmt.Println("⏱️  Latency Distribution:")
	fmt.Printf("   Avg:   %v\n", avgLat)
	fmt.Printf("   P50:   %v\n", p50)
	fmt.Printf("   P99:   %v\n", p99)
	fmt.Printf("   Max:   %v\n", maxLat)
	fmt.Println("--------------------------------------------------")
	
	// 简单的 ASCII 柱状图 (Visualizing Latency)
	printHistogram(sortedLats)

	if *saveResults {
		saveToCSV(elapsed, reqs, qps, avgLat, p99, errs)
	}
}

func saveToCSV(elapsed time.Duration, reqs int64, qps float64, avg, p99 time.Duration, errs int64) {
	filename := "benchmark_results.csv"
	f, err := os.OpenFile(filename, os.O_APPEND|os.O_CREATE|os.O_WRONLY, 0644)
	if err != nil {
		fmt.Printf("❌ Failed to open %s: %v\n", filename, err)
		return
	}
	defer f.Close()

	// 写入表头 (如果是新文件)
	info, _ := f.Stat()
	if info.Size() == 0 {
		fmt.Fprintf(f, "Timestamp,Server Name,Concurrency,Duration(s),Total Reqs,QPS,Avg Latency(ms),P99 Latency(ms),Errors\n")
	}

	timestamp := time.Now().Format("2006-01-02 15:04:05")
	fmt.Fprintf(f, "%s,%s,%d,%.2f,%d,%.2f,%.2f,%.2f,%d\n",
		timestamp,
		*serverName,
		*concurrency,
		elapsed.Seconds(),
		reqs,
		qps,
		float64(avg.Microseconds())/1000.0,
		float64(p99.Microseconds())/1000.0,
		errs,
	)
	fmt.Printf("\n💾 Results saved to %s\n", filename)
}

func printHistogram(lats []time.Duration) {
	if len(lats) == 0 {
		return
	}
	min := lats[0].Seconds() * 1000 // ms
	max := lats[len(lats)-1].Seconds() * 1000 // ms
	bins := 10
	step := (max - min) / float64(bins)
	if step == 0 { step = 1 }

	counts := make([]int, bins)
	for _, l := range lats {
		val := l.Seconds() * 1000
		idx := int((val - min) / step)
		if idx >= bins { idx = bins - 1 }
		counts[idx]++
	}

	fmt.Println("📈 Latency Histogram (ms):")
	maxCount := 0
	for _, c := range counts {
		if c > maxCount { maxCount = c }
	}

	for i := 0; i < bins; i++ {
		start := min + float64(i)*step
		end := min + float64(i+1)*step
		barLen := int(float64(counts[i]) / float64(maxCount) * 40)
		bar := ""
		for k := 0; k < barLen; k++ { bar += "█" }
		if counts[i] > 0 {
			fmt.Printf("   %.2f - %.2f ms : %-40s (%d)\n", start, end, bar, counts[i])
		}
	}
}
