FROM python:3.12-slim

ENV PYTHONDONTWRITEBYTECODE=1
ENV PYTHONUNBUFFERED=1
ENV SIMPLE_DAY_DB=/data/simple_day.sqlite

WORKDIR /app

COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt \
	-i https://pypi.tuna.tsinghua.edu.cn/simple \
	--timeout 60

COPY app ./app

EXPOSE 18080
CMD ["uvicorn", "app.main:app", "--host", "0.0.0.0", "--port", "18080"]
