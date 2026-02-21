package main

import (
	"encoding/csv"
	"fmt"
	"os"
	"strconv"
)

type Result struct {
	ServerName string
	QPS        float64
}

func main() {
	filename := "benchmark_results.csv"
	f, err := os.Open(filename)
	if err != nil {
		fmt.Printf("❌ Cannot open %s: %v\n", filename, err)
		return
	}
	defer f.Close()

	reader := csv.NewReader(f)
	records, err := reader.ReadAll()
	if err != nil {
		fmt.Printf("❌ Error reading CSV: %v\n", err)
		return
	}

	if len(records) < 2 {
		fmt.Println("⚠️  No data found in CSV.")
		return
	}

	// 提取最新的 QPS 数据
	// Map: ServerName -> QPS
	data := make(map[string]float64)
	
	// 从第 2 行开始读取 (跳过表头)
	for i := 1; i < len(records); i++ {
		row := records[i]
		if len(row) < 6 {
			continue
		}
		name := row[1]
		qpsStr := row[5]
		qps, _ := strconv.ParseFloat(qpsStr, 64)
		
		// 覆盖旧数据，只保留最新的
		data[name] = qps
	}

	if len(data) == 0 {
		fmt.Println("⚠️  No valid data extracted.")
		return
	}

	generateSVG(data, "benchmark_chart.svg")
}

func generateSVG(data map[string]float64, filename string) {
	width := 800
	barHeight := 40
	gap := 20
	marginTop := 60
	marginLeft := 200
	marginRight := 50
	
	n := len(data)
	height := marginTop + n*(barHeight+gap) + 50

	f, err := os.Create(filename)
	if err != nil {
		fmt.Printf("❌ Failed to create SVG: %v\n", err)
		return
	}
	defer f.Close()

	// SVG Header
	fmt.Fprintf(f, `<svg width="%d" height="%d" xmlns="http://www.w3.org/2000/svg">`+"\n", width, height)
	fmt.Fprintf(f, `<style>
		.bar { fill: #4CAF50; }
		.bar:hover { fill: #66BB6A; }
		.text { font-family: Arial, sans-serif; font-size: 14px; fill: #333; }
		.title { font-family: Arial, sans-serif; font-size: 20px; font-weight: bold; fill: #333; }
		.qps { font-weight: bold; fill: #fff; }
	</style>`+"\n")
	
	// Background
	fmt.Fprintf(f, `<rect width="100%%" height="100%%" fill="#f9f9f9"/>`+"\n")
	
	// Title
	fmt.Fprintf(f, `<text x="%d" y="40" class="title" text-anchor="middle">Server Performance Comparison (QPS)</text>`+"\n", width/2)

	// Find Max QPS for scaling
	maxQPS := 0.0
	type item struct {
		Name string
		QPS  float64
	}
	var items []item

	for k, v := range data {
		if v > maxQPS {
			maxQPS = v
		}
		items = append(items, item{k, v})
	}

	// Sort by QPS descending
	// Bubble sort for simplicity (n is small)
	for i := 0; i < len(items)-1; i++ {
		for j := 0; j < len(items)-i-1; j++ {
			if items[j].QPS < items[j+1].QPS {
				items[j], items[j+1] = items[j+1], items[j]
			}
		}
	}

	// Draw Bars
	y := marginTop
	for _, it := range items {
		name := it.Name
		qps := it.QPS
		barWidth := int((qps / maxQPS) * float64(width - marginLeft - marginRight))
		if barWidth < 10 { barWidth = 10 } // Min width

		// Label (Server Name)
		fmt.Fprintf(f, `<text x="%d" y="%d" class="text" text-anchor="end" alignment-baseline="middle">%s</text>`+"\n", 
			marginLeft-10, y+barHeight/2, name)
		
		// Bar
		fmt.Fprintf(f, `<rect x="%d" y="%d" width="%d" height="%d" class="bar" rx="4" ry="4"/>`+"\n", 
			marginLeft, y, barWidth, barHeight)
		
		// Value (QPS)
		// 如果条太短，把数字写在外面；否则写在里面
		if barWidth > 60 {
			fmt.Fprintf(f, `<text x="%d" y="%d" class="text qps" text-anchor="end" alignment-baseline="middle">%.0f</text>`+"\n", 
				marginLeft+barWidth-10, y+barHeight/2, qps)
		} else {
			fmt.Fprintf(f, `<text x="%d" y="%d" class="text" text-anchor="start" alignment-baseline="middle">%.0f</text>`+"\n", 
				marginLeft+barWidth+10, y+barHeight/2, qps)
		}

		y += barHeight + gap
	}

	fmt.Fprintf(f, `</svg>`)
	fmt.Printf("✅ Chart generated: %s\n", filename)
}
