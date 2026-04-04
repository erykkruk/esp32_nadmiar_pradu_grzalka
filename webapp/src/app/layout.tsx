import type { Metadata } from "next";
import "./globals.css";

export const metadata: Metadata = {
  title: "Heater Control - ESP32",
  description: "Solar excess heater control dashboard",
};

export default function RootLayout({
  children,
}: {
  children: React.ReactNode;
}) {
  return (
    <html lang="pl">
      <body>{children}</body>
    </html>
  );
}
