# models.py
from sqlalchemy import Column, Integer, String, DateTime, ForeignKey
from sqlalchemy.ext.declarative import declarative_base

Base = declarative_base()

class Chat(Base):
    __tablename__ = 'chats'
    chat_id = Column(Integer, primary_key=True)
    title = Column(String)
    last_updated = Column(DateTime)

class Message(Base):
    __tablename__ = 'messages'
    chat_id = Column(Integer, ForeignKey('chats.chat_id'), primary_key=True)
    message_id = Column(Integer, primary_key=True)
    text = Column(String)
    sender_id = Column(Integer)
    sent_at = Column(DateTime)

class DeletedMessage(Base):
    __tablename__ = 'deleted_messages'
    chat_id = Column(Integer, ForeignKey('chats.chat_id'), primary_key=True)
    message_id = Column(Integer, primary_key=True)
    text = Column(String)
    deleted_at = Column(DateTime)

class MessageHistory(Base):
    __tablename__ = 'message_history'
    chat_id = Column(Integer, ForeignKey('chats.chat_id'), primary_key=True)
    message_id = Column(Integer, primary_key=True)
    version = Column(Integer, primary_key=True)
    text = Column(String)
    edited_at = Column(DateTime)