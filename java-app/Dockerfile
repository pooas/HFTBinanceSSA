# Stage 1: Build the application
FROM maven:3.9-eclipse-temurin-17 AS builder
WORKDIR /app

COPY pom.xml .
# RUN mvn dependency:go-offline -B

COPY src ./src
RUN mvn clean package -DskipTests

# Stage 2: Run the application
FROM eclipse-temurin:17-jre
WORKDIR /app

# کپی کردن دقیق فایلی که نامش را قفل کردیم
COPY --from=builder /app/target/hft-app.jar app.jar

CMD ["java", "-jar", "app.jar"]