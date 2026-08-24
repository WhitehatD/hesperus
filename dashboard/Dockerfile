FROM node:20-alpine AS builder
WORKDIR /app
COPY package*.json ./
RUN npm ci
COPY . .
ARG NEXT_PUBLIC_API_URL=""
ARG NEXT_PUBLIC_MQTT_WS_URL=""
# Read-only MQTT identity (mosquitto/acl grants it `topic read` only, never
# `readwrite`) — see the comment in hooks/useMQTT.ts for why it's safe
# for this specific value to end up in the public JS bundle.
ARG NEXT_PUBLIC_MQTT_USERNAME=""
ARG NEXT_PUBLIC_MQTT_PASSWORD=""
ENV NEXT_PUBLIC_API_URL=$NEXT_PUBLIC_API_URL
ENV NEXT_PUBLIC_MQTT_WS_URL=$NEXT_PUBLIC_MQTT_WS_URL
ENV NEXT_PUBLIC_MQTT_USERNAME=$NEXT_PUBLIC_MQTT_USERNAME
ENV NEXT_PUBLIC_MQTT_PASSWORD=$NEXT_PUBLIC_MQTT_PASSWORD
RUN npm run build

FROM node:20-alpine AS runner
WORKDIR /app
ENV NODE_ENV=production
COPY --from=builder /app/.next/standalone ./
COPY --from=builder /app/.next/static ./.next/static
COPY --from=builder /app/public ./public
EXPOSE 3000
CMD ["node", "server.js"]
